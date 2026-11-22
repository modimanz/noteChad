"""Read MSI tables and extract the embedded CAB. Never invoke Windows Installer actions."""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import tempfile
import warnings

warnings.filterwarnings('ignore', category=DeprecationWarning, module='msilib')
try:
    import msilib
except ImportError as exc:
    raise SystemExit('Use 64-bit Python 3.11/3.12 with msilib.') from exc

ROOT = Path(__file__).resolve().parents[1]


def verify(package, executable):
    """Verify package provenance, safety tables and actual cabinet payload bytes."""
    package, executable = Path(package).resolve(), Path(executable).resolve()
    receipt = json.loads(package.with_suffix('.msi.json').read_text(encoding='utf-8'))
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    if digest(package) != receipt['msi_sha256']:
        raise ValueError('MSI SHA-256 differs from build receipt')
    db = msilib.OpenDatabase(str(package), msilib.MSIDBOPEN_READONLY)
    tables = {}
    for table in ('Property', 'Directory', 'Component', 'FeatureComponents', 'File', 'MsiFileHash',
                  'Registry', 'Shortcut', 'Upgrade', 'LaunchCondition', 'CustomAction', 'Media',
                  'InstallExecuteSequence', 'InstallUISequence', 'AdminExecuteSequence'):
        view = db.OpenView(f'SELECT * FROM `{table}`')
        view.Execute(None)
        tables[table] = []
        while (row := view.Fetch()) is not None:
            tables[table].append([row.GetString(i) for i in range(1, row.GetFieldCount() + 1)])
        view.Close()
    props = dict(tables['Property'])
    if 'ALLUSERS' in props or tables['CustomAction'] or any(row[1] != '1' for row in tables['Registry']):
        raise ValueError('Package is not exclusively current-user/no-custom-actions')
    if len(tables['File']) != 1 or tables['File'][0][2] != 'NODECHAD.EXE|nodeChad.exe':
        raise ValueError('Unexpected MSI payload file list')
    if db.GetSummaryInformation(0).GetProperty(15) != 10:
        raise ValueError('MSI is not marked compressed and non-elevating (LUA)')
    if props['ProductCode'] != receipt['product_code'] or props['ProductVersion'] != receipt['version']:
        raise ValueError('Product identity differs from build receipt')
    del view, row, db
    scratch = Path(os.environ['LOCALAPPDATA']) / 'hermes/cache/scratch'
    scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='nodechad-cab-check-', dir=scratch) as tmp:
        tmp = Path(tmp)
        cab = tmp / 'payload.cab'
        msi = ctypes.WinDLL('msi', use_last_error=True)
        handle = wintypes.UINT
        msi.MsiOpenDatabaseW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR, ctypes.POINTER(handle)]
        msi.MsiDatabaseOpenViewW.argtypes = [handle, wintypes.LPCWSTR, ctypes.POINTER(handle)]
        msi.MsiViewExecute.argtypes = [handle, handle]
        msi.MsiViewFetch.argtypes = [handle, ctypes.POINTER(handle)]
        msi.MsiRecordReadStream.argtypes = [handle, wintypes.UINT, ctypes.c_void_p, ctypes.POINTER(wintypes.DWORD)]
        msi.MsiCloseHandle.argtypes = [handle]
        handles = []
        def checked(result):
            if result:
                raise OSError(result, 'Read-only MSI cabinet access failed')
        try:
            database, view_handle, record = handle(), handle(), handle()
            checked(msi.MsiOpenDatabaseW(str(package), None, ctypes.byref(database)))
            handles.append(database)
            checked(msi.MsiDatabaseOpenViewW(database, "SELECT `Data` FROM `_Streams` WHERE `Name`='payload.cab'", ctypes.byref(view_handle)))
            handles.append(view_handle)
            checked(msi.MsiViewExecute(view_handle, 0))
            checked(msi.MsiViewFetch(view_handle, ctypes.byref(record)))
            handles.append(record)
            with cab.open('wb') as output:
                while True:
                    buffer = ctypes.create_string_buffer(65536)
                    size = wintypes.DWORD(len(buffer))
                    checked(msi.MsiRecordReadStream(record, 1, buffer, ctypes.byref(size)))
                    if not size.value:
                        break
                    output.write(buffer.raw[:size.value])
        finally:
            for opened in reversed(handles):
                msi.MsiCloseHandle(opened)
        target = tmp / 'extracted'
        target.mkdir()
        # SetupAPI is the native Windows cabinet extractor; no installer action runs.
        class CabinetInfo(ctypes.Structure):
            _fields_ = [('NameInCabinet', wintypes.LPCWSTR), ('FileSize', wintypes.DWORD),
                        ('Win32Error', wintypes.DWORD), ('DosDate', wintypes.WORD),
                        ('DosTime', wintypes.WORD), ('DosAttribs', wintypes.WORD),
                        ('FullTargetName', wintypes.WCHAR * 260)]
        callback_type = ctypes.WINFUNCTYPE(wintypes.UINT, ctypes.c_void_p, wintypes.UINT,
                                          ctypes.c_size_t, ctypes.c_size_t)
        @callback_type
        def callback(context, notification, param1, param2):
            if notification == 0x11:  # SPFILENOTIFY_FILEINCABINET
                info = ctypes.cast(param1, ctypes.POINTER(CabinetInfo)).contents
                if info.NameInCabinet != 'NodeChadExe':
                    return 0  # FILEOP_ABORT: never extract unexpected paths
                info.FullTargetName = str(target / 'NodeChadExe')
                return 1  # FILEOP_DOIT
            return 0  # NO_ERROR for informational/extracted notifications
        setupapi = ctypes.WinDLL('setupapi', use_last_error=True)
        setupapi.SetupIterateCabinetW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, callback_type, ctypes.c_void_p]
        setupapi.SetupIterateCabinetW.restype = wintypes.BOOL
        if not setupapi.SetupIterateCabinetW(str(cab), 0, callback, None):
            raise ctypes.WinError(ctypes.get_last_error())
        files = sorted(path.name for path in target.iterdir())
        if files != ['NodeChadExe']:
            raise ValueError(f'Unexpected cabinet file list: {files}')
        extracted = target / 'NodeChadExe'
        extracted.rename(target / 'nodeChad.exe')
        actual = digest(target / 'nodeChad.exe')
        if actual != receipt['payload_sha256'] or actual != digest(executable):
            raise ValueError('Extracted payload SHA-256 differs from source/receipt')
    return {'version': receipt['version'], 'product_code': props['ProductCode'],
            'msi_sha256': receipt['msi_sha256'], 'payload_sha256': actual,
            'payload_hash_match': True, 'payload_files': ['nodeChad.exe'],
            'installer_executed': False, 'extraction': 'read-only MSI stream + SetupIterateCabinetW',
            'tables': tables}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--msi', type=Path, default=ROOT / 'bin/nodeChad-Setup-x64.msi')
    parser.add_argument('--exe', type=Path, default=ROOT / 'bin/nodeChad.exe')
    parser.add_argument('--report', type=Path, help='Optional JSON report path, normally under bin/')
    args = parser.parse_args()
    result = verify(args.msi, args.exe)
    if args.report:
        args.report.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({key: value for key, value in result.items() if key != 'tables'}, indent=2))
