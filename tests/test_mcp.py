"""Real stdio MCP initialization/discovery/calls; synthetic app unless explicitly opted in."""
import asyncio
from contextlib import asynccontextmanager
from functools import wraps
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / 'tools/notechad_mcp.py'
SDK = importlib.util.find_spec('mcp') is not None
REAL_EXE = os.environ.get('NOTECHAD_MCP_EXE')
SCRATCH = Path(os.environ.get('TMPDIR', str(Path.home() / 'AppData/Local/hermes/cache/scratch')))


def connected(method):
    @wraps(method)
    async def run(self):
        async with self.connect():
            await method(self)
    return run


@unittest.skipUnless(SDK, 'optional mcp SDK is not installed')
class MCPTests(unittest.IsolatedAsyncioTestCase):
    @asynccontextmanager
    async def connect(self):
        from mcp import ClientSession, StdioServerParameters
        from mcp.client.stdio import stdio_client
        from contextlib import AsyncExitStack
        SCRATCH.mkdir(parents=True, exist_ok=True)
        self.directory = tempfile.TemporaryDirectory(prefix='notechad-mcp-', dir=SCRATCH)
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.stack = AsyncExitStack()
        if REAL_EXE:
            self.assertTrue(Path(REAL_EXE).is_file(), 'NOTECHAD_MCP_EXE must point to a built executable')
            args = [str(SERVER), '--exe', REAL_EXE]
        else:
            bootstrap = (
                'import sys,runpy; '
                f'sys.path.insert(0,{str(ROOT / "tests")!r}); '
                'import test_mcp_surrogate; '
                'sys.modules["notechad_client"]=test_mcp_surrogate; '
                f'sys.argv=[{str(SERVER)!r}]+sys.argv[1:]; '
                f'runpy.run_path({str(SERVER)!r},run_name="__main__")'
            )
            args = ['-c', bootstrap, '--exe', str(self.root / 'synthetic-app.exe')]
        args += ['--allow-root', str(self.root)]
        try:
            read, write = await self.stack.enter_async_context(stdio_client(
                StdioServerParameters(command=sys.executable, args=args)))
            self.session = await self.stack.enter_async_context(ClientSession(read, write))
            self.initialized = await asyncio.wait_for(self.session.initialize(), 15)
            yield
        finally:
            await self.stack.aclose()

    async def call(self, name, **arguments):
        result = await asyncio.wait_for(self.session.call_tool(name, arguments), 15)
        self.assertEqual(len(result.content), 1)
        return result.model_dump(by_alias=True), json.loads(result.content[0].text)

    @connected
    async def test_initialization_discovery_and_read(self):
        self.assertEqual(self.initialized.model_dump(by_alias=True)['serverInfo']['name'], 'notechad')
        listed = await self.session.list_tools()
        self.assertEqual({t.name for t in listed.tools},
                         {'read', 'write', 'edit', 'save', 'load'})
        for tool in listed.tools:
            if tool.name != 'read':
                self.assertIn('expected_revision', tool.model_dump(by_alias=True)['inputSchema']['required'])
        result, body = await self.call('read')
        self.assertFalse(result['isError'])
        self.assertEqual(body['text'], '')
        self.assertFalse(body['dirty'])
        self.assertIs(type(body['revision']), int)


    @connected
    async def test_mutations_stale_revision_and_temporary_disk_roundtrip(self):
        _, initial = await self.call('read')
        result, written = await self.call('write', expected_revision=initial['revision'], text='A😀B\n世界')
        self.assertFalse(result['isError'], written)
        self.assertTrue(written['dirty'])
        result, stale = await self.call('write', expected_revision=initial['revision'], text='MUST NOT APPLY')
        self.assertTrue(result['isError'])
        self.assertFalse(stale['retryable'])
        _, read = await self.call('read')
        self.assertEqual(read['text'], 'A😀B\n世界')
        result, edited = await self.call('edit', expected_revision=read['revision'], start=1, end=2, text='🦀')
        self.assertFalse(result['isError'], edited)
        destination = self.root / 'roundtrip.txt'
        result, saved = await self.call('save', expected_revision=edited['revision'], path=str(destination))
        self.assertFalse(result['isError'], saved)
        self.assertFalse(saved['dirty'])
        self.assertEqual(destination.read_text(encoding='utf-8'), 'A🦀B\n世界')
        _, current = await self.call('read')
        _, changed = await self.call('write', expected_revision=current['revision'], text='different')
        result, refused = await self.call('load', expected_revision=changed['revision'], path=str(destination))
        self.assertTrue(result['isError'])
        result, saved_change = await self.call('save', expected_revision=changed['revision'], path=str(self.root / 'changed.txt'))
        self.assertFalse(result['isError'], saved_change)
        result, loaded = await self.call('load', expected_revision=saved_change['revision'], path=str(destination))
        self.assertFalse(result['isError'], loaded)
        _, current = await self.call('read')
        self.assertEqual(current['text'], 'A🦀B\n世界')
        self.assertFalse(current['dirty'])
        result, saved = await self.call('save', expected_revision=current['revision'])
        self.assertFalse(result['isError'], saved)


    @connected
    async def test_bounded_pagination_and_strict_arguments(self):
        _, initial = await self.call('read')
        text = '😀\n' * 8005
        result, written = await self.call('write', expected_revision=initial['revision'], text=text)
        self.assertFalse(result['isError'], written)
        _, page = await self.call('read', max_chars=16000)
        self.assertEqual(page['text'], text[:16000])
        self.assertTrue(page['truncated'])
        self.assertEqual(page['next_start'], 16000)
        _, rest = await self.call('read', start=page['next_start'])
        self.assertEqual(rest['text'], text[16000:])
        self.assertFalse(rest['truncated'])
        for name, arguments in [
            ('write', {'text': 'bad'}),
            ('write', {'expected_revision': True, 'text': 'bad'}),
            ('write', {'expected_revision': written['revision'], 'text': '\x00'}),
            ('write', {'expected_revision': written['revision'], 'text': '😀' * 262145}),
            ('read', {'max_chars': 16001}),
            ('read', {'start': -1}),
            ('read', {'unexpected': True}),
            ('edit', {'expected_revision': written['revision'], 'start': 2, 'end': 1, 'text': ''}),
        ]:
            result, error = await self.call(name, **arguments)
            self.assertTrue(result['isError'], (name, error))
            self.assertLess(len(result['content'][0]['text']), 2500)
        result, after = await self.call('read')
        self.assertFalse(result['isError'], after)
        self.assertEqual(after['revision'], written['revision'])

    @connected
    async def test_concurrent_mutations_serialize_without_retry(self):
        _, initial = await self.call('read')
        replies = await asyncio.gather(
            self.call('write', expected_revision=initial['revision'], text='one'),
            self.call('write', expected_revision=initial['revision'], text='two'))
        self.assertEqual(sum(not result['isError'] for result, body in replies), 1)
        _, after = await self.call('read')
        self.assertIn(after['text'], ('one', 'two'))

    @connected
    async def test_path_policy_and_structured_app_error_recovery(self):
        _, initial = await self.call('read')
        for path in ['relative.txt', str(self.root.parent / 'outside.txt')]:
            result, error = await self.call('save', expected_revision=initial['revision'], path=path)
            self.assertTrue(result['isError'], error)
        result, error = await self.call('load', expected_revision=initial['revision'], path=str(self.root / 'missing.txt'))
        self.assertTrue(result['isError'])
        self.assertIn('revision', error)
        result, after = await self.call('read')
        self.assertFalse(result['isError'], after)
        self.assertEqual(after['text'], '')


if __name__ == '__main__':
    unittest.main(verbosity=2)
