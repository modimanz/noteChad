"""Process-scoped Win32 tab acceptance; never sends to another editor PID."""
import ctypes as c
from ctypes import wintypes as w
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
EXE = Path(os.environ.get('NODECHAD_TEST_EXE', ROOT / 'bin/nodeChad.exe')).resolve()
u = c.windll.user32
u.FindWindowExW.argtypes = [w.HWND, w.HWND, w.LPCWSTR, w.LPCWSTR]
u.FindWindowExW.restype = w.HWND
u.SendMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
u.SendMessageW.restype = c.c_ssize_t
u.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
u.IsWindowVisible.argtypes = [w.HWND]
u.GetParent.argtypes = [w.HWND]
u.GetParent.restype = w.HWND

def windows(pid, cls='NodeChad.Editor'):
    result=[]; prior=None
    while True:
        h=u.FindWindowExW(None,prior,cls,None)
        if not h: return result
        prior=h; owner=w.DWORD(); u.GetWindowThreadProcessId(w.HWND(h),c.byref(owner))
        if owner.value==pid: result.append(h)

def text(e):
    b=c.create_unicode_buffer(8192); u.SendMessageW(e,13,len(b),c.addressof(b)); return b.value

def edit_text(e,s):
    b=c.create_unicode_buffer(s); u.SendMessageW(e,0xC2,1,c.addressof(b))

def command(h,n): u.SendMessageW(h,0x111,n,0)
def strip(h): return u.FindWindowExW(h,None,'SysTabControl32',None)
def active(h):
    e=None
    while True:
        e=u.FindWindowExW(h,e,'Edit',None)
        if not e: return None
        if u.IsWindowVisible(e): return e

class TabsCore(unittest.TestCase):
    def launch(self,*paths,api=False):
        p=subprocess.Popen([str(EXE), '--api-window' if api else '--standalone', *map(str,paths)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        def cleanup():
            if p.poll() is None: p.kill()
            p.communicate(timeout=4)
        self.addCleanup(cleanup)
        deadline=time.monotonic()+5
        while time.monotonic()<deadline:
            hs=windows(p.pid)
            if hs and active(hs[0]):
                h=hs[0]
                if api or (strip(h) and u.SendMessageW(strip(h),0x1304,0,0)==max(1,len(paths))):
                    u.SendMessageW(h,0,0,0)
                    return p,h
            time.sleep(.02)
        self.fail('editor not created')

    def test_new_cycle_close_and_move_preserves_native_edit(self):
        p,h=self.launch(); s=strip(h)
        self.assertTrue(s,'normal window must have native tab strip')
        self.assertEqual(u.SendMessageW(s,0x1304,0,0),1)
        e=active(h); edit_text(e,'draft one'); u.SendMessageW(e,0xB1,2,5)
        command(h,106); other=active(h); self.assertNotEqual(other,e)
        self.assertEqual(text(other),''); command(h,111)
        self.assertEqual(active(h),e); self.assertEqual(text(e),'draft one')
        command(h,109)
        hs=windows(p.pid); self.assertEqual(len(hs),2)
        dest=next(x for x in hs if x!=h)
        self.assertEqual(active(dest),e); self.assertEqual(u.GetParent(e),dest)
        a=w.DWORD(); b=w.DWORD(); u.SendMessageW(e,0xB0,c.addressof(a),c.addressof(b))
        self.assertEqual((a.value,b.value),(2,5))
        self.assertTrue(u.SendMessageW(e,0xC6,0,0),'undo retained by transfer')
        command(dest,201); self.assertEqual(text(e),'')
        command(h,107); self.assertEqual(len(windows(p.pid)),1)
        self.assertIsNone(p.poll(),'closing one frame must not quit process')

    def test_multiple_cli_paths_and_per_tab_save(self):
        with tempfile.TemporaryDirectory(dir=os.environ['TMPDIR']) as tmp:
            a=Path(tmp)/'a.md'; b=Path(tmp)/'b.txt'; a.write_bytes(b'first\r\n'); b.write_bytes(b'second\n')
            p,h=self.launch(a,b); self.assertEqual(u.SendMessageW(strip(h),0x1304,0,0),2)
            command(h,111); e=active(h); self.assertEqual(text(e),'first\r\n')
            u.SendMessageW(e,0xB1,len(text(e)),len(text(e))); edit_text(e,'changed'); command(h,103)
            self.assertEqual(a.read_bytes(),b'first\r\nchanged'); self.assertEqual(b.read_bytes(),b'second\n')
            command(h,110); self.assertEqual(text(active(h)),'second\r\n')

    def test_window_cancel_retains_earlier_discarded_drafts(self):
        p,h=self.launch(); first=active(h); edit_text(first,'keep first draft')
        command(h,106); second=active(h); edit_text(second,'keep second draft')
        u.PostMessageW(h,0x10,0,0)
        def dialog():
            deadline=time.monotonic()+3
            while time.monotonic()<deadline:
                ds=windows(p.pid,'#32770')
                if ds: return ds[0]
                time.sleep(.01)
            self.fail('missing save confirmation')
        d=dialog(); self.assertEqual(active(h),first); command(d,7)  # No
        deadline=time.monotonic()+3
        while time.monotonic()<deadline and active(h)!=second: time.sleep(.01)
        command(dialog(),2)  # Cancel a later tab: the first must not be gone.
        u.SendMessageW(h,0,0,0)
        self.assertEqual(u.SendMessageW(strip(h),0x1304,0,0),2)
        self.assertEqual(text(first),'keep first draft'); self.assertEqual(text(second),'keep second draft')
        self.assertIsNone(p.poll()); command(h,111); self.assertEqual(active(h),first)
        self.assertTrue(text(h).startswith('* '),'first draft remains dirty')

    def test_last_tab_move_closes_source_and_modeless_views(self):
        p,h=self.launch(); e=active(h); edit_text(e,'# Header\r\nbody')
        command(h,304); self.assertEqual(len(windows(p.pid,'NodeChad.Markdown')),1)
        command(h,109); hs=windows(p.pid); self.assertEqual(len(hs),1)
        self.assertNotEqual(hs[0],h); self.assertEqual(active(hs[0]),e)
        self.assertFalse(windows(p.pid,'NodeChad.Markdown'))
        self.assertIsNone(p.poll()); self.assertEqual(text(e),'# Header\r\nbody')
        command(hs[0],304); self.assertEqual(len(windows(p.pid,'NodeChad.Markdown')),1)
        command(hs[0],106); self.assertFalse(windows(p.pid,'NodeChad.Markdown'))

    def test_controlled_commands_cannot_add_documents_or_windows(self):
        p,h=self.launch(api=True); e=active(h)
        for n in (101,106,107,108,109,110,111): command(h,n)
        self.assertEqual(windows(p.pid),[h]); self.assertEqual(active(h),e)
        self.assertFalse(strip(h),'controlled editor has no drag or tab surface')

if __name__=='__main__': unittest.main(verbosity=2)
