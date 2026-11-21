"""Dependency-free, synchronous client for one isolated nodeChad editor.

Import NodeChad, launch with a context manager, and use request(). No shell,
network service, background polling, or implicit saving is involved.
"""
from dataclasses import dataclass
import os
import re
import subprocess
import threading

MAX_BYTES = 32 * 1024 * 1024
MAX_HEADER = 128
MAX_INTEGER = (1 << 64) - 1
VERBS = frozenset(('read', 'write', 'edit', 'save', 'load'))
_RESPONSE = re.compile(rb'(OK|ERR) ([0-9]{1,20}) ([01]) ([0-9]{1,8})\n')


@dataclass(frozen=True)
class Response:
    revision: int
    dirty: bool
    text: str


class NodeChadError(Exception):
    """Base for valid editor errors and broken transport errors."""


class ApiError(NodeChadError):
    """An ERR packet; the session remains usable for coordinator errors."""
    def __init__(self, response):
        self.response = response
        self.revision = response.revision
        self.dirty = response.dirty
        self.text = response.text
        super().__init__(response.text)


class ProtocolError(NodeChadError):
    """Malformed/truncated response or failed pipe; do not retry a mutation blindly."""


class NodeChad:
    def __init__(self, exe, visible=False):
        self.exe = os.fspath(exe)
        if not self.exe or not isinstance(visible, bool):
            raise ValueError('exe must name an executable; visible must be bool')
        self.visible = visible
        self.process = None
        self._closed = False
        self._lock = threading.RLock()

    def __enter__(self):
        if self.process is not None or self._closed:
            raise RuntimeError('a NodeChad instance may only be started once')
        self.process = subprocess.Popen(
            [self.exe, '--api-window' if self.visible else '--api'],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, shell=False,
        )
        return self

    def __exit__(self, exc_type, exc, traceback):
        self.close()
        return False

    def close(self):
        """Send EOF. Hidden instances exit/discard; visible instances stay open.

        Hidden shutdown is bounded; an unresponsive owned child is terminated.
        No mode saves automatically. Keep process to observe a visible child.
        """
        with self._lock:
            if self._closed:
                return
            self._closed = True
            if self.process is None:
                return
            try:
                self.process.stdin.close()
            except (OSError, ValueError):
                pass
            if not self.visible:
                try:
                    self.process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.process.terminate()
                    try:
                        self.process.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        self.process.kill()
                        self.process.wait()
            self.process.stdout.close()

    def _read_exact(self, count):
        chunks = []
        remaining = count
        while remaining:
            chunk = self.process.stdout.read(remaining)
            if not chunk:
                raise ProtocolError('truncated response payload')
            chunks.append(chunk)
            remaining -= len(chunk)
        return b''.join(chunks)

    def request(self, verb, rev=0, start=0, end=0, text=''):
        """Issue read/write/edit/save/load and return Response(revision, dirty, text).

        Non-read revisions must equal the live revision (read first). Edit offsets
        count Unicode code points in LF-normalized text, end-exclusive. For
        save/load, text is a path; save with empty text uses the current path.
        """
        if not isinstance(verb, str) or verb not in VERBS:
            raise ValueError('unsupported verb')
        for name, value in (('rev', rev), ('start', start), ('end', end)):
            if type(value) is not int or not 0 <= value <= MAX_INTEGER:
                raise ValueError(name + ' must be an unsigned 64-bit integer')
        if not isinstance(text, str) or '\x00' in text:
            raise ValueError('text must be a string without NUL')
        try:
            payload = text.encode('utf-8', errors='strict')
        except UnicodeEncodeError as error:
            raise ValueError('text contains an unpaired surrogate') from error
        if len(payload) > MAX_BYTES:
            raise ValueError('payload exceeds 32 MiB')
        if verb == 'read' and (rev or start or end or payload):
            raise ValueError('read requires revision/offsets zero and empty text')
        if verb != 'edit' and (start or end):
            raise ValueError('only edit accepts offsets')
        if verb == 'edit' and start > end:
            raise ValueError('edit start exceeds end')
        if verb == 'load' and not payload:
            raise ValueError('load requires a path')
        header = '{} {} {} {} {}\n'.format(verb, rev, start, end, len(payload)).encode('ascii')
        with self._lock:
            if self.process is None or self._closed:
                raise RuntimeError('enter the NodeChad context before making requests')
            try:
                self.process.stdin.write(header)
                self.process.stdin.write(payload)
                self.process.stdin.flush()
                line = self.process.stdout.readline(MAX_HEADER + 1)
                match = _RESPONSE.fullmatch(line)
                if not match or len(line) > MAX_HEADER:
                    raise ProtocolError('invalid or truncated response header')
                status, revision, dirty, size = match.groups()
                revision, size = int(revision), int(size)
                if revision > MAX_INTEGER or size > MAX_BYTES:
                    raise ProtocolError('response limit exceeded')
                body = self._read_exact(size)
                if b'\0' in body:
                    raise ProtocolError('NUL in response payload')
                try:
                    decoded = body.decode('utf-8', errors='strict')
                except UnicodeDecodeError as error:
                    raise ProtocolError('invalid UTF-8 response') from error
                if status == b'OK' and verb != 'read' and body:
                    raise ProtocolError('successful mutation response must be empty')
                response = Response(revision, dirty == b'1', decoded)
            except (OSError, ValueError, ProtocolError) as error:
                self.close()
                if isinstance(error, ProtocolError):
                    raise
                raise ProtocolError('pipe failure; mutation outcome may be unknown') from error
            if status == b'ERR':
                raise ApiError(response)
            return response
