"""Two independent launches of a private copy must share one GUI process."""
import ctypes as c
from ctypes import wintypes as w
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT=Path(__file__).resolve().parents[1]
EXE=Path(os.environ.get('NODECHAD_TEST_EXE',ROOT/'bin/nodeChad.exe')).resolve()
u=c.windll.user32
u.FindWindowExW.argtypes=[w.HWND,w.HWND,w.LPCWSTR,w.LPCWSTR]; u.FindWindowExW.restype=w.HWND

def frames(pid):
    prev=None; out=[]
    while True:
        h=u.FindWindowExW(None,prev,'NodeChad.Editor',None)
        if not h: return out
        p=w.DWORD(); u.GetWindowThreadProcessId(w.HWND(h),c.byref(p)); prev=h
        if p.value==pid and u.IsWindowVisible(w.HWND(h)): out.append(h)

def wait_for(get):
    deadline=time.monotonic()+6
    while time.monotonic()<deadline:
        value=get()
        if value:return value
        time.sleep(.03)
    raise AssertionError('expected GUI condition not reached')

class RouterTest(unittest.TestCase):
    def test_second_launch_joins_first_with_its_own_cwd(self):
        with tempfile.TemporaryDirectory() as temp:
            d=Path(temp); exe=d/'test editor.exe'; shutil.copyfile(EXE,exe)
            first=subprocess.Popen([str(exe)],cwd=d); second=None
            try:
                wait_for(lambda:frames(first.pid)); sub=d/'folder'; sub.mkdir(); (sub/'hello.md').write_text('demo')
                second=subprocess.Popen([str(exe),'hello.md'],cwd=sub)
                wait_for(lambda: len(frames(first.pid))==2); self.assertEqual(second.wait(4),0)
                titles=[]
                for h in frames(first.pid):
                    b=c.create_unicode_buffer(256);u.GetWindowTextW(w.HWND(h),b,256);titles.append(b.value)
                self.assertTrue(any('hello.md' in s for s in titles),titles)
                for h in frames(first.pid): u.PostMessageW(w.HWND(h),0x10,0,0)
                self.assertEqual(first.wait(4),0)
            finally:
                for p in [second,first]:
                    if p and p.poll() is None:p.kill();p.wait(5)

if __name__=='__main__':unittest.main(verbosity=2)
