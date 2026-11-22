"""Black-box acceptance against the built executable. No third-party modules."""
import ctypes as c
from ctypes import wintypes as w
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
EXE = Path(os.environ.get('NOTECHAD_TEST_EXE', ROOT / 'bin/noteChad.exe')).resolve()
u = c.windll.user32
u.FindWindowExW.argtypes = [w.HWND, w.HWND, w.LPCWSTR, w.LPCWSTR]
u.FindWindowExW.restype = w.HWND
u.SendMessageW.argtypes = [w.HWND,w.UINT,w.WPARAM,w.LPARAM]
u.SendMessageW.restype = c.c_ssize_t

class EditorTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(EXE.is_file(), 'compile bin/noteChad.exe first')
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def launch(self, *args):
        p = subprocess.Popen([str(EXE), *map(str,args)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=self.root)
        def cleanup():
            if p.poll() is None: p.kill()
            p.communicate(timeout=4)
        self.addCleanup(cleanup)
        return p

    def exchange(self, p, verb, rev=0, text='', start=0, end=0, ok=True):
        data=text.encode('utf8'); p.stdin.write(f'{verb} {rev} {start} {end} {len(data)}\n'.encode()+data); p.stdin.flush()
        header=p.stdout.readline().split(); self.assertEqual(len(header),4,header)
        state,current,dirty,size=header; body=p.stdout.read(int(size)).decode('utf8')
        self.assertEqual(state,b'OK' if ok else b'ERR',body)
        return int(current),int(dirty),body

    def editor(self,p,visible=True):
        hwnd=None; deadline=time.monotonic()+5
        while time.monotonic()<deadline and not hwnd:
            prior=None
            while True:
                h=u.FindWindowExW(None,prior,'NoteChad.Editor',None)
                if not h: break
                pid=w.DWORD(); u.GetWindowThreadProcessId(w.HWND(h),c.byref(pid)); prior=h
                if pid.value==p.pid and bool(u.IsWindowVisible(w.HWND(h)))==visible: hwnd=h; break
            if not hwnd: time.sleep(.03)
        self.assertTrue(hwnd,'editor did not launch'); self.assertEqual(bool(u.IsWindowVisible(w.HWND(hwnd))),visible)
        edit=u.FindWindowExW(hwnd,None,'Edit',None); self.assertTrue(edit)
        return hwnd,edit

    def test_window_shows_exact_product_name(self):
        p = self.launch('--standalone'); hwnd, _ = self.editor(p)
        caption = c.create_unicode_buffer(256)
        u.GetWindowTextW(w.HWND(hwnd), caption, len(caption))
        self.assertTrue(caption.value.endswith('noteChad'), caption.value)

    def test_hermes_workflow_and_conflict(self):
        src=self.root/'first note.md'; src.write_bytes('# Café\ncat🐈 jumps\n'.encode())
        p=self.launch('--api'); rev,d,_=self.exchange(p,'read'); self.assertFalse(d)
        rev,d,_=self.exchange(p,'load',rev,str(src)); rev,_,body=self.exchange(p,'read'); self.assertEqual(body,src.read_text(encoding='utf-8'))
        old=rev; i=body.index(' jumps'); rev,d,_=self.exchange(p,'edit',rev,' naps',i,i+6); self.assertTrue(d)
        self.exchange(p,'write',old,'stale',ok=False); self.exchange(p,'load',rev,str(src),ok=False)
        rev,d,_=self.exchange(p,'save',rev); self.assertFalse(d); self.assertEqual(src.read_bytes(),'# Café\ncat🐈 naps\n'.encode())
        rev,_,_=self.exchange(p,'write',rev,'draft\n🌲'); other=self.root/'untitled 📘.md'; other.write_text('keep')
        self.exchange(p,'save',rev,str(other),ok=False); self.assertEqual(other.read_text(),'keep')
        src.write_text('outside editor'); self.exchange(p,'save',rev,ok=False); self.assertEqual(src.read_text(),'outside editor')
        fresh=self.root/'draft.md'; rev,_,_=self.exchange(p,'save',rev,str(fresh)); self.assertEqual(fresh.read_text(encoding='utf8'),'draft\n🌲')
        self.exchange(p,'edit',rev,'bad',999,999,ok=False); self.assertEqual(self.exchange(p,'read')[2],'draft\n🌲')
        p.stdin.close(); p.stdin=None; self.assertEqual(p.wait(timeout=5),0)

    def test_window_and_api_share_unsaved_buffer(self):
        p=self.launch('--api-window'); rev,_,_=self.exchange(p,'read'); h,e=self.editor(p)
        text=c.create_unicode_buffer('working'); u.SendMessageW(e,0xC2,1,c.addressof(text))  # EM_REPLACESEL
        self.exchange(p,'write',rev,'outdated',ok=False); rev,d,s=self.exchange(p,'read'); self.assertEqual((d,s),(1,'working'))
        u.SendMessageW(e,0xB1,0,7); u.SendMessageW(h,0x111,301,0)  # Markdown > Bold
        rev,_,s=self.exchange(p,'read'); self.assertEqual(s,'**working**')
        rev,_,_=self.exchange(p,'write',rev,'shared'); b=c.create_unicode_buffer(32); u.SendMessageW(e,0xD,32,c.addressof(b)); self.assertEqual(b.value,'shared')
        p.stdin.close(); p.stdin=None; self.assertIsNone(p.poll()); self.assertTrue(u.IsWindow(w.HWND(h)))

    def test_undo_formatted_selection_and_agent_replacement(self):
        p=self.launch('--api-window'); rev,_,_=self.exchange(p,'read'); h,e=self.editor(p)
        rev,_,_=self.exchange(p,'write',rev,'a paragraph'); u.SendMessageW(e,0xB1,2,11)
        u.SendMessageW(h,0x111,301,0); self.assertEqual(self.exchange(p,'read')[2],'a **paragraph**')
        u.SendMessageW(h,0x111,201,0); rev,_,body=self.exchange(p,'read'); self.assertEqual(body,'a paragraph')
        self.exchange(p,'write',rev,'agent replaced all'); u.SendMessageW(h,0x111,201,0)
        self.assertEqual(self.exchange(p,'read')[2],'a paragraph')

    def test_classic_launch_edit_save_reopen(self):
        path=self.root/'demo.md'; path.write_bytes(b'# Plan\r\n'); p=self.launch(path); h,e=self.editor(p)
        u.SendMessageW(e,0xB1,8,8); text=c.create_unicode_buffer('- [ ] verify'); u.SendMessageW(e,0xC2,1,c.addressof(text)); u.SendMessageW(h,0x111,103,0)
        self.assertEqual(path.read_bytes(),b'# Plan\r\n- [ ] verify'); u.PostMessageW(w.HWND(h),0x10,0,0); self.assertEqual(p.wait(timeout=5),0)
        again=self.launch(path); _,e=self.editor(again); b=c.create_unicode_buffer(80); u.SendMessageW(e,0xD,80,c.addressof(b)); self.assertEqual(b.value,'# Plan\r\n- [ ] verify')

if __name__=='__main__': unittest.main(verbosity=2)
