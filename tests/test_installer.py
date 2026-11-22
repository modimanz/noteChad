"""Installer qualification: no install/registration or elevation is performed."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
import warnings

warnings.filterwarnings('ignore', category=DeprecationWarning, module='msilib')
ROOT = Path(__file__).resolve().parents[1]


class InstallerTests(unittest.TestCase):
    def test_builds_single_file_per_user_msi(self):
        script = ROOT / 'installer/build.py'
        self.assertTrue(script.is_file(), 'installer/build.py must build the MSI')
        spec = importlib.util.spec_from_file_location('nodechad_installer', script)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        import msilib
        scratch = Path(os.environ['LOCALAPPDATA']) / 'hermes/cache/scratch'
        scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='nodechad-msi-', dir=scratch) as tmp:
            output = Path(tmp) / 'setup.msi'
            module.build(ROOT / 'bin/nodeChad.exe', output, '0.1.0')
            db = msilib.OpenDatabase(str(output), msilib.MSIDBOPEN_READONLY)
            def rows(sql):
                view = db.OpenView(sql)
                view.Execute(None)
                result = []
                while (row := view.Fetch()) is not None:
                    result.append(tuple(row.GetString(i) for i in range(1, row.GetFieldCount() + 1)))
                view.Close()
                return result
            props = dict(rows('SELECT `Property`, `Value` FROM `Property`'))
            self.assertNotIn('ALLUSERS', props)
            self.assertEqual(props['ProductVersion'], '0.1.0')
            self.assertEqual(rows('SELECT `FileName` FROM `File`'), [('NODECHAD.EXE|nodeChad.exe',)])
            self.assertEqual(rows('SELECT `Root` FROM `Registry`'), [('1',)] * len(rows('SELECT `Registry` FROM `Registry`')))
            directories = rows('SELECT `Directory`, `Directory_Parent`, `DefaultDir` FROM `Directory`')
            self.assertIn(('INSTALLDIR', 'ProgramsDir', 'nodeChad'), directories)
            self.assertIn(('ProgramsDir', 'LocalAppDataFolder', 'Programs'), directories)
            self.assertEqual(rows('SELECT `Directory_`, `Target` FROM `Shortcut`'), [('ProgramMenuFolder', '[#NodeChadExe]')])
            self.assertEqual(db.GetSummaryInformation(0).GetProperty(7), b'x64;1033')
            self.assertEqual(db.GetSummaryInformation(0).GetProperty(15), 10)
            self.assertEqual(rows('SELECT `Action` FROM `CustomAction`'), [])
            self.assertIn(('RegisterProduct',), rows('SELECT `Action` FROM `InstallExecuteSequence`'))
            registry = rows('SELECT `Key`, `Name`, `Value` FROM `Registry`')
            self.assertIn(('Software\\Classes\\Applications\\nodeChad.exe\\shell\\open\\command', '', '"[#NodeChadExe]" "%1"'), registry)
            self.assertFalse(any('UserChoice' in key or (key.endswith(('.md', '.txt')) and not name) for key, name, value in registry))
            self.assertEqual(len(rows('SELECT `UpgradeCode` FROM `Upgrade`')), 2)
            orders = [order for action, condition, order in rows('SELECT * FROM `InstallExecuteSequence`')]
            self.assertEqual(len(orders), len(set(orders)), 'MSI execute action order must be unambiguous')
            self.assertIn(('RemoveExistingProducts', '1510'), rows('SELECT `Action`, `Sequence` FROM `InstallExecuteSequence`'))
            del db

    def test_isolated_cabinet_extraction_matches_input(self):
        script = ROOT / 'installer/verify.py'
        self.assertTrue(script.is_file(), 'installer/verify.py must verify the embedded payload without installing')
        spec = importlib.util.spec_from_file_location('nodechad_verify', script)
        verifier = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(verifier)
        builder_spec = importlib.util.spec_from_file_location('nodechad_build', ROOT / 'installer/build.py')
        builder = importlib.util.module_from_spec(builder_spec)
        builder_spec.loader.exec_module(builder)
        scratch = Path(os.environ['LOCALAPPDATA']) / 'hermes/cache/scratch'
        with tempfile.TemporaryDirectory(prefix='nodechad-extract-test-', dir=scratch) as tmp:
            output = Path(tmp) / 'setup.msi'
            builder.build(ROOT / 'bin/nodeChad.exe', output, '0.1.1')
            result = verifier.verify(output, ROOT / 'bin/nodeChad.exe')
            self.assertTrue(result['payload_hash_match'])
            self.assertEqual(result['payload_files'], ['nodeChad.exe'])
            self.assertFalse(result['installer_executed'])


if __name__ == '__main__':
    unittest.main()
