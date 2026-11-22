"""Build and exercise the real Win32 transport against a GUI-thread stub."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRATCH = Path(os.environ['LOCALAPPDATA']) / 'hermes/cache/scratch'


class TransportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        SCRATCH.mkdir(parents=True, exist_ok=True)
        cls.work = tempfile.TemporaryDirectory(prefix='notechad-api-', dir=SCRATCH)
        cls.addClassCleanup(cls.work.cleanup)
        cls.exe = Path(cls.work.name) / 'transport fixture.exe'
        vswhere = Path(os.environ['ProgramFiles(x86)']) / 'Microsoft Visual Studio/Installer/vswhere.exe'
        install = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires',
                                          'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                                          '-property', 'installationPath'], text=True).strip()
        vcvars = Path(install) / 'VC/Auxiliary/Build/vcvars64.bat'
        batch = Path(cls.work.name) / 'build.cmd'
        batch.write_text('@echo off\ncall "{}" >nul && cl /nologo /W4 /WX /O1 /MT /TC '
                         '/Fe:"{}" "{}" "{}" /link user32.lib\n'.format(
                             vcvars, cls.exe, ROOT / 'tests/test_api_transport_stub.c', ROOT / 'src/api.c'))
        built = subprocess.run(['cmd.exe', '/d', '/c', str(batch)], cwd=cls.work.name,
                               capture_output=True, text=True)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    def exchange(self, data):
        result = subprocess.run([str(self.exe), '--api'], input=data, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def test_exact_32_mib_packet_reaches_coordinator(self):
        size = 32 * 1024 * 1024
        output = self.exchange(b'write 1 0 0 33554432\n' + b'x' * size)
        self.assertEqual(output, b'ERR 1 0 13\nstub capacity')

    def test_transport_error_reports_initial_live_revision(self):
        self.assertTrue(self.exchange(b'exec 0 0 0 0\n').startswith(b'ERR 1 0 '))

    def test_malformed_packets_are_terminal_without_dispatch(self):
        bad = [
            b'exec 1 0 0 0\n', b'read 0 0 0 33554433\n',
            b'read 18446744073709551616 0 0 0\n', b'read -1 0 0 0\n',
            b'read +0 0 0 0\n', b'read  0 0 0 0\n', b'read 0 0 0 0\r\n',
            b'read 0 0 0 0 extra\n', b'read 0 0 0 0\x00\n',
            b'x' * 128 + b'\n', b'read 1 0 0 0\n', b'read 0 0 0 1\nx',
            b'write 1 1 0 0\n', b'edit 1 3 2 0\n', b'load 1 0 0 0\n',
            b'write 1 0 0 1\n\x00', b'write 1 0 0 1\n\xff',
            b'write 1 0 0 3\n\xed\xa0\x80', b'write 1 0 0 2\n\xc0\xaf',
            b'write 1 0 0 4\n\xf4\x90\x80\x80',
            b'write 1 0 0 3\nxy', b'read 0 0',
        ]
        for packet in bad:
            with self.subTest(packet=packet[:60]):
                out = self.exchange(b'read 0 0 0 0\n' + packet)
                self.assertTrue(out.startswith(b'OK 1 0 0\nERR 1 0 '), out)
                header, body = out[len(b'OK 1 0 0\n'):].split(b'\n', 1)
                self.assertEqual(len(body), int(header.split()[-1]))
                self.assertTrue(body.decode('utf-8'))

    def test_fragmented_packets_and_empty_eof(self):
        self.assertEqual(self.exchange(b''), b'')
        process = subprocess.Popen([str(self.exe), '--api'], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        try:
            for chunk in [b'w', b'rite 1 ', b'0 0 4', b'\n', b'\xf0', b'\x9f\x98', b'\x80', b'read 0 0 0 0\n']:
                process.stdin.write(chunk)
                process.stdin.flush()
            output, _ = process.communicate(timeout=10)
            self.assertEqual(output, b'OK 2 1 0\nOK 2 1 4\n\xf0\x9f\x98\x80')
            self.assertEqual(process.returncode, 0)
        finally:
            if process.poll() is None:
                process.kill()
            process.wait()

    def test_all_five_verbs_reach_coordinator(self):
        for verb, payload, start, end in [('edit', b'x', 0, 0), ('save', b'', 0, 0),
                                           ('save', b'target.txt', 0, 0), ('load', b'target.txt', 0, 0)]:
            with self.subTest(verb=verb, payload=payload):
                packet = '{} 1 {} {} {}\n'.format(verb, start, end, len(payload)).encode() + payload
                out = self.exchange(packet)
                self.assertTrue(out.startswith(b'ERR 1 0 '), out)
                self.assertTrue(out.endswith(b'stub only implements read/write'), out)

    def test_python_client_interface_and_structured_conflict(self):
        import importlib.util
        import sys
        path = ROOT / 'tools/notechad_client.py'
        self.assertTrue(path.is_file(), 'Python client is missing')
        spec = importlib.util.spec_from_file_location('notechad_client', path)
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        with module.NoteChad(self.exe, visible=False) as nc:
            initial = nc.request('read')
            self.assertEqual((initial.revision, initial.dirty, initial.text), (1, False, ''))
            written = nc.request('write', rev=initial.revision, text='hello\n世界 😀')
            self.assertEqual((written.revision, written.dirty, written.text), (2, True, ''))
            with self.assertRaises(module.ApiError) as error:
                nc.request('write', rev=1, text='must not appear')
            self.assertEqual((error.exception.revision, error.exception.dirty), (2, True))
            self.assertIn('revision', error.exception.text)
            self.assertEqual(nc.request('read').text, 'hello\n世界 😀')
        self.assertEqual(nc.process.poll(), 0)

    def test_read_write_same_document_on_gui_thread(self):
        text = 'hello\n世界 😀'.encode('utf-8')
        wire = b'read 0 0 0 0\nwrite 1 0 0 ' + str(len(text)).encode() + b'\n' + text + b'read 0 0 0 0\n'
        self.assertEqual(self.exchange(wire), b'OK 1 0 0\nOK 2 1 0\nOK 2 1 ' + str(len(text)).encode() + b'\n' + text)


if __name__ == '__main__':
    unittest.main(verbosity=2)
