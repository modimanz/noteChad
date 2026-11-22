"""Client-side admission and hostile-response tests (stdlib only)."""
import importlib.util
import io
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest

PATH = Path(__file__).resolve().parents[1] / 'tools/notechad_client.py'
SPEC = importlib.util.spec_from_file_location('notechad_client', PATH)
client = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = client
SPEC.loader.exec_module(client)


class ClientTests(unittest.TestCase):
    def session(self, wire):
        nc = client.NoteChad('unused-fixture.exe', visible=True)
        nc.process = SimpleNamespace(stdin=io.BytesIO(), stdout=io.BytesIO(wire))
        self.addCleanup(nc.close)
        return nc

    def test_invalid_arguments_never_write_to_pipe(self):
        nc = self.session(b'')
        bad = [dict(verb='exec'), dict(verb='read', rev=1), dict(verb='read', text='x'),
               dict(verb='write', rev=True), dict(verb='write', rev=-1),
               dict(verb='write', rev=1 << 64), dict(verb='write', start=1),
               dict(verb='edit', start=2, end=1), dict(verb='load'),
               dict(verb='write', text='\0'), dict(verb='write', text='\ud800'),
               dict(verb='write', text='\udfff'), dict(verb='write', text='x' * (client.MAX_BYTES + 1))]
        for args in bad:
            with self.subTest(args={k: (v[:20] if isinstance(v, str) else v) for k, v in args.items()}):
                with self.assertRaises(ValueError):
                    nc.request(**args)
        self.assertEqual(nc.process.stdin.getvalue(), b'')

    def test_malformed_responses_close_session(self):
        bad = [b'', b'OK 1 0 0\r\n', b'OK 1 2 0\n', b'OK -1 0 0\n', b'OK 1 0 33554433\n',
               b'OK 18446744073709551616 0 0\n', b'OK 1 0 1\n', b'OK 1 0 1\n\0',
               b'OK 1 0 3\n\xed\xa0\x80', b'OK 1 0 1\n\xff', b'x' * 129 + b'\n']
        for wire in bad:
            with self.subTest(wire=wire[:60]):
                nc = self.session(wire)
                with self.assertRaises(client.ProtocolError):
                    nc.request('read')
                self.assertTrue(nc._closed)

    def test_mutation_success_body_must_be_empty(self):
        nc = self.session(b'OK 2 1 1\nx')
        with self.assertRaises(client.ProtocolError):
            nc.request('save', rev=1)

    def test_error_fields_and_next_response(self):
        nc = self.session(b'ERR 7 1 8\nconflictOK 7 1 4\ntext')
        with self.assertRaises(client.ApiError) as caught:
            nc.request('write', rev=3)
        self.assertEqual((caught.exception.revision, caught.exception.dirty, caught.exception.text), (7, True, 'conflict'))
        self.assertEqual(nc.request('read'), client.Response(7, True, 'text'))

    def test_wire_counts_utf8_bytes_not_codepoints(self):
        nc = self.session(b'OK 9 1 0\n')
        nc.request('edit', rev=8, start=2, end=3, text='😀')
        self.assertEqual(nc.process.stdin.getvalue(), b'edit 8 2 3 4\n\xf0\x9f\x98\x80')

    def test_context_required_and_no_implicit_restart(self):
        nc = client.NoteChad('unused-fixture.exe')
        with self.assertRaises(RuntimeError):
            nc.request('read')
        nc.close()
        with self.assertRaises(RuntimeError):
            nc.__enter__()


if __name__ == '__main__':
    unittest.main(verbosity=2)
