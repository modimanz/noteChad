"""Optional stdio MCP server. The standalone GUI does not import this module."""
from __future__ import annotations

import argparse
import asyncio
from contextlib import ExitStack
import json
from pathlib import Path
import threading

MAX_TEXT_BYTES = 1024 * 1024
MAX_READ_CHARS = 16000
MAX_ERROR_CHARS = 1000
REVISION = {'type': 'integer', 'minimum': 0,
            'description': 'Exact revision from read in this bridge session. Never guess or automatically retry stale mutations.'}
TEXT = {'type': 'string', 'maxLength': MAX_TEXT_BYTES,
        'description': 'UTF-8 text; at most 1 MiB encoded. LF newlines recommended.'}
PATH = {'type': 'string', 'maxLength': 32767,
        'description': 'Absolute file path inside a configured --allow-root. File access is disabled without an allowed root.'}


def tool_specs():
    def spec(name, description, properties, required):
        return dict(name=name, description=description,
                    inputSchema={'type': 'object', 'properties': properties,
                                 'required': required, 'additionalProperties': False})
    return [
        spec('read', 'Read LF-normalized buffer text and revision. Pages use Unicode code points. Read again after any error; never replay a mutation automatically.',
             {'start': {'type': 'integer', 'minimum': 0, 'default': 0},
              'max_chars': {'type': 'integer', 'minimum': 1, 'maximum': MAX_READ_CHARS, 'default': MAX_READ_CHARS}}, []),
        spec('write', 'Replace the entire buffer, not the disk file. Requires exact expected_revision from read; stale errors are never retried.',
             {'expected_revision': REVISION, 'text': TEXT}, ['expected_revision', 'text']),
        spec('edit', 'Replace [start,end) using Unicode code points, not UTF-8 bytes or UTF-16 units. start=end inserts. Requires exact expected_revision; never auto-retry.',
             {'expected_revision': REVISION, 'start': {'type': 'integer', 'minimum': 0},
              'end': {'type': 'integer', 'minimum': 0}, 'text': TEXT},
             ['expected_revision', 'start', 'end', 'text']),
        spec('save', 'Save buffer to disk; may overwrite a file. Empty path uses the last bridge-approved load/save path. Requires exact expected_revision; never auto-retry.',
             {'expected_revision': REVISION, 'path': {**PATH, 'default': ''}}, ['expected_revision']),
        spec('load', 'Replace buffer from an absolute allowed file. Dirty buffers must be saved first. Requires exact expected_revision; never auto-retry.',
             {'expected_revision': REVISION, 'path': PATH}, ['expected_revision', 'path']),
    ]


class Bridge:
    """Own one lazily created editor. Serialize lifecycle and all requests."""
    def __init__(self, exe, *, visible=False, allow_roots=(), client_factory=None):
        self.exe, self.visible = str(exe), visible
        self.roots = tuple(Path(p).resolve(strict=True) for p in allow_roots)
        self.factory = client_factory
        self.lock = threading.Lock()
        self.stack = ExitStack()
        self.client = None
        self.current_path = None
        self.closed = False
        self.failed = False

    def _client(self):
        if self.closed or self.failed:
            raise ValueError('Editor session unavailable. Restart the bridge and read; do not replay a previous mutation.')
        if self.client is None:
            factory = self.factory
            if factory is None:
                from nodechad_client import NodeChad
                factory = NodeChad
            self.client = self.stack.enter_context(factory(self.exe, visible=self.visible))
        return self.client

    def close(self):
        with self.lock:
            self.closed = True
            self.stack.close()

    def _request(self, verb, **kwargs):
        try:
            return self._client().request(verb, **kwargs)
        except Exception as exc:
            if not hasattr(exc, 'revision'):
                self.failed = True  # No implicit respawn or reuse after an unknown outcome.
            raise

    def _path(self, value):
        if not self.roots:
            raise ValueError('Disk access disabled. Configure --allow-root before load/save.')
        path = Path(value)
        if not value or '\x00' in value or not path.is_absolute():
            raise ValueError('Path must be absolute and nonempty')
        path = path.resolve()
        if not any(path.is_relative_to(root) for root in self.roots):
            raise ValueError('Path is outside configured --allow-root directories')
        return str(path)

    def call(self, name, arguments):
        with self.lock:
            try:
                self._validate(name, arguments)
                response = self._request('read', rev=0, start=0, end=0, text='')
                if name == 'read':
                    start = arguments.get('start', 0)
                    text = response.text[start:start + arguments.get('max_chars', MAX_READ_CHARS)]
                    next_start = start + len(text)
                    return False, {'revision': response.revision, 'dirty': response.dirty,
                                   'text': text, 'start': start, 'total_chars': len(response.text),
                                   'truncated': next_start < len(response.text),
                                   'next_start': next_start if next_start < len(response.text) else None}
                if response.revision != arguments['expected_revision']:
                    return True, {'error': 'stale_revision', 'revision': response.revision,
                                  'dirty': response.dirty, 'retryable': False,
                                  'action': 'Read current text and reassess. Do not substitute this revision and replay.'}
                payload = arguments.get('text', '')
                path = None
                if name in ('save', 'load'):
                    value = arguments.get('path', '')
                    if name == 'save' and not value:
                        value = self.current_path or ''
                    path = self._path(value)
                    payload = path
                    if name == 'load' and response.dirty:
                        raise ValueError('Dirty buffer: save it before loading another file. No implicit discard is supported.')
                if name == 'edit' and not 0 <= arguments['start'] <= arguments['end'] <= len(response.text):
                    raise ValueError('Invalid edit range for current Unicode code-point length')
                response = self._request(name, rev=arguments['expected_revision'],
                                         start=arguments.get('start', 0), end=arguments.get('end', 0), text=payload)
                if path is not None:
                    self.current_path = path
                return False, {'revision': response.revision, 'dirty': response.dirty, 'text': ''}
            except Exception as exc:
                body = {'error': str(exc)[:MAX_ERROR_CHARS], 'retryable': False,
                        'action': 'Read current state and reassess intent. Never automatically retry a mutation.'}
                if hasattr(exc, 'revision'):
                    body.update(revision=exc.revision, dirty=exc.dirty)
                if self.failed:
                    body['action'] = 'Outcome may be unknown. Restart bridge, read, and manually reconcile; never replay blindly.'
                return True, body

    def _validate(self, name, arguments):
        specs = {item['name']: item for item in tool_specs()}
        if name not in specs:
            raise ValueError('Unknown tool')
        if not isinstance(arguments, dict):
            raise ValueError('Arguments must be an object')
        schema = specs[name]['inputSchema']
        if set(arguments) - set(schema['properties']):
            raise ValueError('Unknown argument')
        for required in schema['required']:
            if required not in arguments:
                raise ValueError(f'Missing required argument: {required}')
        for key, value in arguments.items():
            rule = schema['properties'][key]
            wanted = {'integer': int, 'string': str, 'boolean': bool}[rule['type']]
            if type(value) is not wanted:
                raise ValueError(f'{key} must be {rule["type"]}')
            if wanted is int and not rule.get('minimum', 0) <= value <= rule.get('maximum', 2**64 - 1):
                raise ValueError(f'{key} outside allowed range')
            if wanted is str:
                if '\x00' in value:
                    raise ValueError(f'{key} must not contain NUL')
                try:
                    encoded_size = len(value.encode('utf-8', errors='strict'))
                except UnicodeEncodeError:
                    raise ValueError(f'{key} must contain valid Unicode, not unpaired surrogates') from None
                if len(value) > rule['maxLength'] or encoded_size > MAX_TEXT_BYTES:
                    raise ValueError(f'{key} exceeds size limit')


def create_server(bridge):
    """Support official Python MCP SDK 1.x and 2.x handler interfaces."""
    import anyio
    from mcp.server import Server
    from mcp import types

    tools = [types.Tool(**item, annotations=types.ToolAnnotations(
        readOnlyHint=item['name'] == 'read', destructiveHint=item['name'] != 'read',
        idempotentHint=False, openWorldHint=False)) for item in tool_specs()]

    async def invoke(name, arguments):
        error, payload = await anyio.to_thread.run_sync(bridge.call, name, arguments or {})
        return types.CallToolResult(isError=error, content=[types.TextContent(
            type='text', text=json.dumps(payload, ensure_ascii=False))])

    if hasattr(Server, 'call_tool'):
        server = Server('nodechad', version='1.0.0')

        @server.list_tools()
        async def list_tools():
            return tools

        @server.call_tool(validate_input=False)
        async def call_tool(name, arguments):
            return await invoke(name, arguments)
    else:
        async def list_tools(context, params):
            return types.ListToolsResult(tools=tools)

        async def call_tool(context, params):
            return await invoke(params.name, params.arguments)

        server = Server('nodechad', version='1.0.0', on_list_tools=list_tools, on_call_tool=call_tool)
    return server


async def serve(bridge):
    from mcp.server.stdio import stdio_server
    server = create_server(bridge)
    async with stdio_server() as (read, write):
        await server.run(read, write, server.create_initialization_options())


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', required=True, help='Absolute path to bin/nodeChad.exe')
    parser.add_argument('--allow-root', action='append', default=[], help='Existing directory allowed for load/save; repeatable. Default: no disk access.')
    parser.add_argument('--visible', action='store_true', help='Use --api-window; closing MCP leaves the visible editor open.')
    args = parser.parse_args(argv)
    if not Path(args.exe).is_absolute():
        parser.error('--exe must be an absolute path')
    if any(not Path(p).is_absolute() or not Path(p).is_dir() for p in args.allow_root):
        parser.error('--allow-root must be an existing absolute directory')
    try:
        import mcp  # noqa: F401 -- optional, only the bridge needs it
    except ImportError:
        parser.exit(2, 'Optional MCP SDK missing. Use a Python with mcp installed, or install mcp in a separate bridge venv.\n')
    bridge = Bridge(args.exe, visible=args.visible, allow_roots=args.allow_root)
    try:
        asyncio.run(serve(bridge))
    finally:
        bridge.close()


if __name__ == '__main__':
    main()
