"""Use the actual tab-strip mouse handler; never move the desktop pointer."""
import ctypes as c
from ctypes import wintypes as w
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import time
import unittest
from test_router import frames, wait_for, EXE
u=c.windll.user32
u.FindWindowExW.argtypes=[w.HWND,w.HWND,w.LPCWSTR,w.LPCWSTR]; u.FindWindowExW.restype=w.HWND
u.SendMessageW.argtypes=[w.HWND,w.UINT,w.WPARAM,w.LPARAM];u.SendMessageW.restype=c.c_ssize_t
u.GetParent.argtypes=[w.HWND];u.GetParent.restype=w.HWND
u.SetWindowPos.argtypes=[w.HWND,w.HWND,c.c_int,c.c_int,c.c_int,c.c_int,w.UINT]

def msg(h,m,a=0,b=0):return u.SendMessageW(h,m,a,b)
def point(x,y):return (x&65535)|((y&65535)<<16)
def active_edit(h):
    prev=None
    while True:
        e=u.FindWindowExW(h,prev,'Edit',None)
        if not e or u.IsWindowVisible(w.HWND(e)):return e
        prev=e

def strip(h):return u.FindWindowExW(h,None,'SysTabControl32',None)
def title(h):
    b=c.create_unicode_buffer(512);u.GetWindowTextW(w.HWND(h),b,512);return b.value

def replace(e,text):
    b=c.create_unicode_buffer(text);msg(e,0xc2,1,c.addressof(b))

class DragIntegration(unittest.TestCase):
    def test_move_dirty_unicode_file_between_independent_launches(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);exe=root/'isolated-editor.exe';shutil.copyfile(EXE,exe)
            a=root/'left.md';b=root/'移動.md';a.write_bytes(b'left\n');b.write_bytes(b'# right\n')
            p=subprocess.Popen([str(exe),str(b),str(a)]);q=None
            try:
                source=wait_for(lambda:next(iter(frames(p.pid)),None)); bar=strip(source);self.assertTrue(bar,'no native tab strip')
                self.assertEqual(msg(bar,0x1304),2)
                if b.name not in title(source): msg(source,0x111,110,0)
                self.assertIn(b.name,title(source));e=active_edit(source)
                msg(e,0xb1,9,9);replace(e,'draft');msg(e,0xb1,9,14)
                q=subprocess.Popen([str(exe)]);target=wait_for(lambda:next((h for h in frames(p.pid) if h!=source),None));self.assertEqual(q.wait(5),0)
                dest=strip(target); before=msg(dest,0x1304); self.assertGreaterEqual(before,1)
                width=min(360,(u.GetSystemMetrics(0)-24)//2);self.assertGreater(width,160)
                u.SetWindowPos(source,w.HWND(-1),6,18,width,230,0x10);u.SetWindowPos(target,w.HWND(-1),width+12,18,width,230,0x10)
                # Document b is in slot zero. Use real client coordinates and let
                # the tab control perform its own hit test (no foreign pointers).
                start=w.POINT(23,10)
                end=w.POINT(22,10);u.ClientToScreen(w.HWND(dest),c.byref(end));u.ScreenToClient(w.HWND(bar),c.byref(end))
                msg(bar,0x201,1,point(start.x,start.y));msg(bar,0x200,1,point(start.x+14,start.y));msg(bar,0x200,1,point(end.x,end.y));msg(bar,0x202,0,point(end.x,end.y))
                wait_for(lambda:u.GetParent(e)==target);self.assertEqual(active_edit(target),e);self.assertIn('*',title(target));self.assertIn(b.name,title(target))
                self.assertEqual(msg(strip(source),0x1304),1);self.assertEqual(msg(dest,0x1304),before+1)
                sel=msg(e,0xb0);self.assertEqual((sel&65535,(sel>>16)&65535),(9,14));msg(target,0x111,103);self.assertEqual(b.read_bytes(),b'# right\ndraft');self.assertEqual(a.read_bytes(),b'left\n')
                msg(target,0x111,201);content=c.create_unicode_buffer(64);msg(e,13,64,c.addressof(content));self.assertEqual(content.value,'# right\r\n')
            finally:
                for owned in [q,p]:
                    if owned and owned.poll() is None:owned.kill();owned.wait(5)

if __name__=='__main__':unittest.main(verbosity=2)
