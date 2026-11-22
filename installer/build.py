"""Build an unsigned x64, strictly per-user Windows Installer package.

Requires 64-bit CPython 3.11/3.12 with stdlib msilib. No downloaded tools.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import tempfile
import uuid
import warnings

warnings.filterwarnings('ignore', category=DeprecationWarning, module='msilib')
try:
    import msilib
    from msilib import schema, sequence
except ImportError as exc:
    raise SystemExit('Use 64-bit CPython 3.11 or 3.12 with msilib (for example: py -3.11 installer\\build.py).') from exc

ROOT = Path(__file__).resolve().parents[1]
UPGRADE_CODE = '{9A209231-AFB0-4614-AC71-DC87D9A96687}'
COMPONENT_CODE = '{33600F3D-40C1-43E4-A9DC-B55FA86233E7}'
NAMESPACE = uuid.UUID(UPGRADE_CODE.strip('{}'))


def build(executable, output, version):
    """Create MSI plus a SHA-256 provenance receipt without installing anything."""
    if not re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)', version):
        raise ValueError('Version must be three canonical integers: major.minor.build')
    if any(n > maximum for n, maximum in zip(map(int, version.split('.')), (255, 255, 65535))):
        raise ValueError('MSI version limits are 255.255.65535')
    if struct.calcsize('P') != 8:
        raise ValueError('Use 64-bit Python to build the x64 package')
    executable, output = Path(executable).resolve(), Path(output).resolve()
    image = executable.read_bytes()
    if image[:2] != b'MZ' or len(image) < 64:
        raise ValueError('Input is not a Windows PE executable')
    pe_offset = struct.unpack_from('<I', image, 60)[0]
    if image[pe_offset:pe_offset + 6] != b'PE\0\0\x64\x86':
        raise ValueError('Input must be an AMD64 PE executable')
    if output.suffix.lower() != '.msi' or output == executable:
        raise ValueError('Output must be an .msi file, not the input')
    output.parent.mkdir(parents=True, exist_ok=True)
    product_code = '{' + str(uuid.uuid5(NAMESPACE, 'nodeChad-x64-' + version)).upper() + '}'
    # Build in an owned child directory; publish the complete package only on success.
    with tempfile.TemporaryDirectory(prefix='nodechad-package-', dir=output.parent) as tmp:
        candidate = Path(tmp) / output.name
        db = msilib.init_database(str(candidate), schema, 'nodeChad', product_code, version, 'nodeChad')
        add = lambda table, rows: msilib.add_data(db, table, rows)
        summary = db.GetSummaryInformation(20)
        summary.SetProperty(msilib.PID_TEMPLATE, 'x64;1033')
        summary.SetProperty(msilib.PID_WORDCOUNT, 10)  # compressed + LUA: never request elevation
        summary.SetProperty(msilib.PID_PAGECOUNT, 200)
        summary.Persist()
        add('Property', [
            ('UpgradeCode', UPGRADE_CODE), ('INSTALLLEVEL', '1'),
            ('ARPCOMMENTS', 'Per-user nodeChad editor. Unsigned package.'),
            ('ARPNOMODIFY', '1'),
            ('SecureCustomProperties', 'OLDPRODUCTS;NEWERPRODUCTFOUND'),
        ])
        add('Directory', [
            ('TARGETDIR', None, 'SourceDir'),
            ('LocalAppDataFolder', 'TARGETDIR', '.'),
            ('ProgramsDir', 'LocalAppDataFolder', 'Programs'),
            ('INSTALLDIR', 'ProgramsDir', 'nodeChad'),
            ('ProgramMenuFolder', 'TARGETDIR', '.'),
        ])
        add('Feature', [('Editor', None, 'nodeChad', 'nodeChad editor', 1, 1, 'INSTALLDIR', 0)])
        # HKCU key path is required for a component that contains per-user resources.
        add('Component', [('EditorComponent', COMPONENT_CODE, 'INSTALLDIR', 260, None, 'InstallMarker')])
        add('FeatureComponents', [('Editor', 'EditorComponent')])
        add('File', [('NodeChadExe', 'EditorComponent', 'NODECHAD.EXE|nodeChad.exe', len(image), None, None, 512, 1)])
        import ctypes
        class FileHash(ctypes.Structure):
            _fields_ = [('size', ctypes.c_uint32), ('parts', ctypes.c_int32 * 4)]
        filehash = FileHash()
        filehash.size = ctypes.sizeof(filehash)
        result = ctypes.windll.msi.MsiGetFileHashW(str(executable), 0, ctypes.byref(filehash))
        if result:
            raise OSError(result, 'MsiGetFileHashW failed')
        add('MsiFileHash', [('NodeChadExe', 0, *filehash.parts)])
        app = r'Software\Classes\Applications\nodeChad.exe'
        add('Registry', [
            ('InstallMarker', 1, r'Software\nodeChad\Installer', 'InstallPath', '[INSTALLDIR]', 'EditorComponent'),
            ('FriendlyName', 1, app, 'FriendlyAppName', 'nodeChad', 'EditorComponent'),
            ('OpenCommand', 1, app + r'\shell\open\command', '', '"[#NodeChadExe]" "%1"', 'EditorComponent'),
            ('SupportedMD', 1, app + r'\SupportedTypes', '.md', '', 'EditorComponent'),
            ('SupportedTXT', 1, app + r'\SupportedTypes', '.txt', '', 'EditorComponent'),
        ])
        add('Shortcut', [('StartMenu', 'ProgramMenuFolder', 'nodeChad', 'EditorComponent', '[#NodeChadExe]', None,
                          'nodeChad editor', None, None, None, 1, 'INSTALLDIR')])
        add('Upgrade', [
            (UPGRADE_CODE, None, version, None, 1, None, 'OLDPRODUCTS'),
            (UPGRADE_CODE, version, None, None, 258, None, 'NEWERPRODUCTFOUND'),
        ])
        add('LaunchCondition', [
            ('NOT ALLUSERS', 'nodeChad supports current-user installation only. Do not set ALLUSERS.'),
            ('VersionNT64', 'nodeChad requires 64-bit Windows.'),
            ('Installed OR NOT NEWERPRODUCTFOUND', 'A newer nodeChad version is already installed. Uninstall it first to downgrade.'),
        ])
        # No executable custom actions, machine registration, services or shell commands.
        execute = [(action, condition, 25 if action == 'FindRelatedProducts' else 1510 if action == 'RemoveExistingProducts' else order)
                   for action, condition, order in sequence.InstallExecuteSequence]
        add('InstallExecuteSequence', execute)
        add('InstallUISequence', [(action, condition, 25 if action == 'FindRelatedProducts' else order)
                                  for action, condition, order in sequence.InstallUISequence])
        add('AdminExecuteSequence', sequence.AdminExecuteSequence)
        add('AdminUISequence', sequence.AdminUISequence)
        cabfile = Path(tmp) / 'payload.cab'
        msilib.FCICreate(str(cabfile), [(str(executable), 'NodeChadExe')])
        add('Media', [(1, 1, None, '#payload.cab', None, None)])
        msilib.add_stream(db, 'payload.cab', str(cabfile))
        db.Commit()
        del summary, db
        candidate.replace(output)
    receipt = {'product': 'nodeChad', 'version': version, 'product_code': product_code,
               'upgrade_code': UPGRADE_CODE, 'signed': False, 'scope': 'current-user',
               'payload': 'nodeChad.exe', 'payload_sha256': hashlib.sha256(image).hexdigest(),
               'msi_sha256': hashlib.sha256(output.read_bytes()).hexdigest()}
    output.with_suffix('.msi.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    return receipt


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version', required=True, help='MSI major.minor.build; increment for every release')
    parser.add_argument('--exe', type=Path, default=ROOT / 'bin/nodeChad.exe')
    parser.add_argument('--output', type=Path, default=ROOT / 'bin/nodeChad-Setup-x64.msi')
    args = parser.parse_args()
    print(json.dumps(build(args.exe, args.output, args.version), indent=2))
