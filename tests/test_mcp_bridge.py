"""Bridge ownership/default-policy tests; no optional SDK required."""
from pathlib import Path
import os
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from nodechad_mcp import Bridge
from test_mcp_surrogate import NodeChad

SCRATCH = Path(os.environ.get('TMPDIR', str(Path.home() / 'AppData/Local/hermes/cache/scratch')))


class BridgeTests(unittest.TestCase):
    def setUp(self):
        SCRATCH.mkdir(parents=True, exist_ok=True)
        directory = tempfile.TemporaryDirectory(prefix='nodechad-mcp-policy-', dir=SCRATCH)
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)

    def bridge(self, **kwargs):
        bridge = Bridge(self.root / 'synthetic.exe', client_factory=NodeChad, **kwargs)
        self.addCleanup(bridge.close)
        return bridge

    def test_lazy_start_and_close_never_reopens(self):
        bridge = self.bridge()
        self.assertIsNone(bridge.client)
        error, result = bridge.call('read', {})
        self.assertFalse(error, result)
        process = bridge.client.process
        bridge.close()
        self.assertIsNotNone(process.poll())
        error, result = bridge.call('read', {})
        self.assertTrue(error)
        self.assertIs(bridge.client.process, process)

    def test_default_disk_access_is_disabled(self):
        bridge = self.bridge()
        _, state = bridge.call('read', {})
        target = self.root / 'not-created.txt'
        error, result = bridge.call('save', {'expected_revision': state['revision'], 'path': str(target)})
        self.assertTrue(error)
        self.assertIn('--allow-root', result['error'])
        self.assertFalse(target.exists())

    def test_invalid_input_is_rejected_before_start(self):
        bridge = self.bridge()
        for args in [None, [], {'expected_revision': True, 'text': 'x'},
                     {'expected_revision': 1, 'text': '\ud800'},
                     {'expected_revision': 1, 'text': '\x00'}]:
            error, result = bridge.call('write', args)
            self.assertTrue(error, result)
            self.assertIsNone(bridge.client)

    def test_unknown_transport_failure_latches_without_respawn(self):
        count = []
        class FailedClient:
            def __init__(self, *args, **kwargs):
                count.append('spawn')
            def __enter__(self):
                return self
            def __exit__(self, *args):
                count.append('close')
            def request(self, *args, **kwargs):
                count.append('request')
                raise OSError('synthetic broken pipe')
        bridge = Bridge(self.root / 'inert.exe', client_factory=FailedClient)
        self.addCleanup(bridge.close)
        self.assertTrue(bridge.call('read', {})[0])
        self.assertTrue(bridge.call('write', {'expected_revision': 1, 'text': 'never'})[0])
        self.assertEqual(count, ['spawn', 'request'])

    def test_symlink_escape_is_rejected(self):
        bridge = self.bridge(allow_roots=[self.root])
        outside = tempfile.TemporaryDirectory(prefix='nodechad-mcp-outside-', dir=SCRATCH)
        self.addCleanup(outside.cleanup)
        alias = self.root / 'escape'
        try:
            alias.symlink_to(Path(outside.name), target_is_directory=True)
        except OSError as exc:
            self.skipTest(f'Host does not permit test-owned symlink creation: {exc}')
        _, state = bridge.call('read', {})
        error, result = bridge.call('save', {'expected_revision': state['revision'],
                                             'path': str(alias / 'not-created.txt')})
        self.assertTrue(error, result)
        self.assertFalse((Path(outside.name) / 'not-created.txt').exists())


if __name__ == '__main__':
    unittest.main(verbosity=2)
