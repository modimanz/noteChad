"""Dependency-free MSVC test runner; only synthetic loopback traffic."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
VCVARS = Path(os.environ.get('NODECHAD_VCVARS', r'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'))

class AITests(unittest.TestCase):
    def test_native_ai_md(self):
        self._native_ai('MD')

    def test_native_ai_mt(self):
        self._native_ai('MT')

    def _native_ai(self, crt):
        self.assertTrue((ROOT / 'src/ai.c').is_file(), 'AI implementation is missing')
        with tempfile.TemporaryDirectory(prefix='nodechad-ai-', dir=os.environ.get('TMPDIR')) as work:
            exe = Path(work) / 'test_ai.exe'
            flags = f'/nologo /W4 /WX /O1 /{crt} /TC /DUNICODE /D_UNICODE'
            command = f'call "{VCVARS}" >nul && cl {flags} /c /Fo:"{Path(work) / "ai.obj"}" "{ROOT / "src/ai.c"}" && cl {flags} /Fe:"{exe}" /Fo:"{Path(work) / "test_ai.obj"}" "{ROOT / "tests/test_ai.c"}" /link user32.lib winhttp.lib'
            batch = Path(work) / 'build.cmd'
            batch.write_text('@echo off\n' + command + '\n', encoding='utf-8')
            build = subprocess.run(['cmd.exe', '/d', '/c', str(batch)], cwd=work, capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            import http.server
            import json
            import threading
            import time
            requests = []
            class Fixture(http.server.BaseHTTPRequestHandler):
                def log_message(self, *args):
                    pass
                def do_POST(self):
                    body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                    requests.append((self.path, body))
                    model = body['model']
                    time.sleep(0.3)
                    status = 200
                    payload = b'{"response":"world\\r\\n\\u03a9","done":true}'
                    if model == 'fixture-http': status, payload = 404, b'{"error":"model missing"}'
                    if model == 'fixture-redirect': status = 302
                    if model == 'fixture-json': payload = b'{"response":"oops",'
                    if model == 'fixture-large': payload = b'x' * 70000
                    if model == 'fixture-utf8': payload = b'{"response":"\xff","done":true}'
                    if model == 'fixture-nul': payload = b'{"response":"bad\\u0000tail","done":true}'
                    if model == 'fixture-duplicate': payload = b'{"response":"a","response":"b","done":true}'
                    if model == 'fixture-incomplete': payload = b'{"response":"no","done":false}'
                    self.send_response(status)
                    if status == 302: self.send_header('Location', '/must-not-follow')
                    self.send_header('Content-Length', str(len(payload)))
                    self.end_headers()
                    try: self.wfile.write(payload)
                    except (ConnectionError, OSError): pass
                def do_GET(self):
                    requests.append((self.path, {}))
                    self.send_error(500)
            server = http.server.HTTPServer(('127.0.0.1', 0), Fixture)
            thread = threading.Thread(target=server.serve_forever)
            thread.start()
            env = os.environ.copy()
            env['NODECHAD_OLLAMA_PORT'] = str(server.server_port)
            try:
                run = subprocess.run([str(exe)], env=env, capture_output=True, text=True, timeout=30)
            finally:
                server.shutdown()
                thread.join(5)
                server.server_close()
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertEqual(len(requests), 17, requests)
            for path, body in requests:
                self.assertEqual(path, '/api/generate')
                self.assertFalse(body['stream'])
                self.assertLessEqual(len(body['prompt'].encode('utf-16-le')) // 2, 2048)
                self.assertEqual(body['prompt'], 'x' * 2048 if body['model'] == 'fixture-context' else 'Hello ')
            print(run.stdout, end='')

if __name__ == '__main__':
    unittest.main(verbosity=2)
