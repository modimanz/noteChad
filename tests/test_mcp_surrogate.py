"""Inert, synthetic test app and agreed-client-interface adapter. Not the GUI."""
import os
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace


class AppError(RuntimeError):
    def __init__(self, response):
        super().__init__(response.text)
        self.revision, self.dirty, self.text = response.revision, response.dirty, response.text


class NodeChad:
    """Test-only adapter: a Python wire surrogate, never a real user editor."""
    def __init__(self, exe, visible=False):
        self.process = None

    def __enter__(self):
        self.process = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), '--app'],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        return self

    def __exit__(self, *args):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=5)
        self.process.stdout.close()

    def request(self, verb, rev=0, start=0, end=0, text=''):
        data = text.encode('utf-8')
        self.process.stdin.write(f'{verb} {rev} {start} {end} {len(data)}\n'.encode('ascii') + data)
        self.process.stdin.flush()
        status, revision, dirty, length = self.process.stdout.readline().split()
        response = SimpleNamespace(revision=int(revision), dirty=bool(int(dirty)),
                                   text=self.process.stdout.read(int(length)).decode('utf-8'))
        if status != b'OK':
            raise AppError(response)
        return response


def app():
    text, revision, dirty, current = '', 1, False, None
    while header := sys.stdin.buffer.readline():
        verb, rev, start, end, length = header.decode('ascii').split()
        rev, start, end, length = map(int, (rev, start, end, length))
        payload = sys.stdin.buffer.read(length).decode('utf-8')
        status, body = 'OK', ''
        try:
            if verb == 'read':
                body = text
            elif rev != revision:
                raise ValueError('stale revision; read before editing')
            elif verb == 'write':
                text = payload.replace('\r\n', '\n').replace('\r', '\n')
                revision += 1
                dirty = True
            elif verb == 'edit':
                if not 0 <= start <= end <= len(text):
                    raise ValueError('invalid code-point range')
                text = text[:start] + payload + text[end:]
                revision += 1
                dirty = True
            elif verb == 'save':
                destination = payload or current
                if not destination:
                    raise ValueError('no current path')
                Path(destination).write_bytes(text.encode('utf-8'))
                current, dirty = destination, False
            elif verb == 'load':
                text = Path(payload).read_text(encoding='utf-8').replace('\r\n', '\n').replace('\r', '\n')
                current, dirty = payload, False
                revision += 1
            else:
                raise ValueError('unknown verb')
        except (ValueError, OSError) as exc:
            status, body = 'ERR', str(exc)
        data = body.encode('utf-8')
        sys.stdout.buffer.write(f'{status} {revision} {int(dirty)} {len(data)}\n'.encode('ascii') + data)
        sys.stdout.buffer.flush()


if __name__ == '__main__' and '--app' in sys.argv:
    app()
