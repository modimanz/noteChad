"""End-to-end tests on a private, standalone noteChad process and scratch notes."""
import ctypes as c
from ctypes import wintypes as w
from pathlib import Path
import tempfile
import time
import unittest
import test_tabs_core as gui

u = gui.u
u.GetDlgItem.argtypes = [w.HWND, c.c_int]; u.GetDlgItem.restype = w.HWND
u.GetWindowRect.argtypes = [w.HWND, c.POINTER(w.RECT)]

def rect(hwnd):
    r = w.RECT(); assert u.GetWindowRect(hwnd, c.byref(r)); return r

def wait_for(predicate):
    limit = time.monotonic() + 4
    while time.monotonic() < limit:
        value = predicate()
        if value: return value
        time.sleep(.02)
    raise AssertionError('timed out waiting for ' + predicate.__name__)

def pane(h):
    p = u.FindWindowExW(h, None, 'RICHEDIT50W', None)
    return p if p and u.IsWindowVisible(p) else None

def point(x, y): return (x & 65535) | ((y & 65535) << 16)

def focused(frame):
    class Info(c.Structure):
        _fields_=[('cb',w.DWORD),('flags',w.DWORD),('active',w.HWND),('focus',w.HWND),('capture',w.HWND),('menu',w.HWND),('move',w.HWND),('caret',w.HWND),('rect',w.RECT)]
    info=Info(); info.cb=c.sizeof(info)
    assert u.GetGUIThreadInfo(u.GetWindowThreadProcessId(w.HWND(frame),None),c.byref(info))
    return info.focus

class SplitWorkspace(unittest.TestCase):
    launch = gui.TabsCore.launch

    def test_live_inline_markdown_follows_document_without_modifying_source(self):
        with tempfile.TemporaryDirectory() as tmp:
            a=Path(tmp)/'Plans.MD'; b=Path(tmp)/'plain.txt'
            a.write_bytes(b'# Plans\n'); b.write_bytes(b'plain')
            proc,h=self.launch(b,a); e=gui.active(h)
            p=wait_for(lambda:pane(h)); self.assertFalse(gui.windows(proc.pid,'NoteChad.Markdown'))
            self.assertEqual(gui.text(e),'# Plans\r\n'); self.assertNotIn('* ',gui.text(h))
            self.assertLessEqual(rect(e).right,rect(p).left); self.assertEqual(rect(e).top,rect(p).top)
            self.assertIn('Plans',gui.text(p)); self.assertNotIn('#',gui.text(p))
            end=len(gui.text(e)); u.SendMessageW(e,0xb1,end,end); gui.edit_text(e,'**Budget**'); sel=u.SendMessageW(e,0xb0,0,0)
            wait_for(lambda:'Budget' in gui.text(p)); self.assertNotIn('**',gui.text(p)); self.assertEqual(a.read_bytes(),b'# Plans\n')
            self.assertEqual(u.SendMessageW(e,0xb0,0,0),sel); self.assertTrue(u.SendMessageW(e,0xc6,0,0))
            gui.command(h,111); self.assertIsNone(pane(h)); self.assertEqual(gui.text(gui.active(h)),'plain')
            gui.command(h,110); p=wait_for(lambda:pane(h)); wait_for(lambda:'Budget' in gui.text(p))
            gui.command(h,305); self.assertIsNone(pane(h)); self.assertEqual(e,gui.active(h))
            gui.command(h,305); p=wait_for(lambda:pane(h)); self.assertEqual(u.SendMessageW(e,0xb0,0,0),sel)
            gui.command(h,109); dest=next(x for x in gui.windows(proc.pid) if x!=h)
            self.assertEqual(gui.active(dest),e); p=wait_for(lambda:pane(dest)); wait_for(lambda:'Budget' in gui.text(p)); self.assertIsNone(pane(h))
            gui.command(dest,103); self.assertEqual(a.read_bytes(),b'# Plans\n**Budget**'); self.assertEqual(b.read_bytes(),b'plain')

    def test_dirty_background_close_can_cancel_or_save_just_that_tab(self):
        with tempfile.TemporaryDirectory() as tmp:
            a=Path(tmp)/'first.txt'; b=Path(tmp)/'second.txt'; a.write_bytes(b'one'); b.write_bytes(b'two')
            proc,h=self.launch(a,b); selected=gui.active(h); gui.command(h,111); first=gui.active(h)
            end=len(gui.text(first)); u.SendMessageW(first,0xb1,end,end); gui.edit_text(first,'!'); gui.command(h,110)
            for choice in [2,6]:  # Cancel, then Yes; both keep the original foreground tab.
                u.PostMessageW(h,0x8000+61,0,gui.strip(h)); dlg=wait_for(lambda:next(iter(gui.windows(proc.pid,'#32770')),None))
                self.assertEqual(gui.active(h),first); gui.command(dlg,choice); u.SendMessageW(h,0,0,0)
                self.assertEqual(gui.active(h),selected); self.assertEqual(b.read_bytes(),b'two')
                if choice==2:
                    self.assertEqual(u.SendMessageW(gui.strip(h),0x1304,0,0),2); self.assertEqual(a.read_bytes(),b'one'); self.assertEqual(gui.text(first),'one!')
            self.assertEqual(a.read_bytes(),b'one!'); self.assertEqual(u.SendMessageW(gui.strip(h),0x1304,0,0),1)

    def test_preview_explains_oversized_notes_and_recovers(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'long.md'; path.write_bytes(b'x\n' * 65537)
            _,h=self.launch(path); e=gui.active(h); p=wait_for(lambda:pane(h))
            self.assertIn('128K',gui.text(p)); self.assertNotIn('* ',gui.text(h))
            u.SendMessageW(e,0xb1,0,-1); gui.edit_text(e,'# Short again')
            wait_for(lambda:'Short again' in gui.text(p)); self.assertNotIn('#',gui.text(p))
            self.assertEqual(path.read_bytes(),b'x\n' * 65537)

    def test_cancel_save_as_does_not_close_unnamed_background_note(self):
        proc,h=self.launch(); first=gui.active(h); gui.edit_text(first,'keep this draft')
        gui.command(h,106); other=gui.active(h); gui.edit_text(other,'keep this too')
        u.PostMessageW(h,0x8000+61,0,gui.strip(h)); prompt=wait_for(lambda:next(iter(gui.windows(proc.pid,'#32770')),None))
        u.PostMessageW(prompt,0x111,6,0)  # Yes -> Save As for the untitled document.
        dialog=wait_for(lambda:next((d for d in gui.windows(proc.pid,'#32770') if u.GetDlgItem(d,1148)),None))
        u.PostMessageW(dialog,0x111,2,0); wait_for(lambda:gui.active(h)==other and not gui.windows(proc.pid,'#32770'))
        self.assertEqual(gui.text(other),'keep this too'); self.assertEqual(gui.text(first),'keep this draft')
        self.assertEqual(u.SendMessageW(gui.strip(h),0x1304,0,0),2); gui.command(h,111); self.assertTrue(gui.text(h).startswith('* '))

    def test_divider_resizes_and_resets_without_changing_text(self):
        _,h=self.launch(); e=gui.active(h); gui.edit_text(e,'# Here'); gui.command(h,305)
        p=wait_for(lambda:pane(h)); divider=u.GetDlgItem(h,903); self.assertTrue(divider)
        before=rect(e).right; u.SendMessageW(divider,0x201,1,point(2,10)); u.SendMessageW(divider,0x200,1,point(82,10)); u.SendMessageW(divider,0x202,0,point(2,10))
        self.assertGreater(rect(e).right,before+35); self.assertLessEqual(rect(e).right,rect(p).left)
        u.SendMessageW(divider,0x203,1,point(2,10)); self.assertAlmostEqual(rect(e).right,before,delta=2)
        self.assertEqual(gui.text(e),'# Here')

    def test_preview_focus_and_width_commands_leave_source_alone(self):
        _,h=self.launch(); e=gui.active(h); gui.edit_text(e,'# Title'); gui.command(h,305); p=wait_for(lambda:pane(h))
        gui.command(h,306); self.assertEqual(focused(h),p); gui.command(h,205)  # select all in preview only
        self.assertNotEqual(u.SendMessageW(p,0xb0,0,0),0); self.assertEqual(gui.text(e),'# Title'); gui.command(h,306); self.assertEqual(focused(h),e)
        before=rect(p).left; gui.command(h,307); self.assertLess(rect(p).left,before-5)
        gui.command(h,308); gui.command(h,308); self.assertGreater(rect(p).left,before+5)
        gui.command(h,309); self.assertAlmostEqual(rect(p).left,before,delta=2)
        u.SetWindowPos(w.HWND(h),None,0,0,565,410,0x16); self.assertLessEqual(rect(e).right,rect(p).left); self.assertEqual(rect(e).top,rect(p).top)

    def test_visible_api_gui_open_replaces_preview(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'loaded note.md'; path.write_text('# Opened from dialog',encoding='utf-8')
            proc,h=self.launch(api=True); gui.command(h,305); p=wait_for(lambda:pane(h)); e=gui.active(h)
            from test_editor import EditorTests
            exchange=lambda *args,**kwargs: EditorTests.exchange(self,proc,*args,**kwargs)
            revision,_,_=exchange('read'); revision,_,_=exchange('write',revision,'# API draft')
            wait_for(lambda:'API draft' in gui.text(p)); revision,_,_=exchange('save',revision,str(Path(tmp)/'api-draft.md'))
            u.PostMessageW(h,0x111,102,0); dlg=wait_for(lambda:next(iter(gui.windows(proc.pid,'#32770')),None))
            _,_,refusal=exchange('write',revision,'must not replace the draft',ok=False); self.assertIn('dialog',refusal)
            self.assertEqual(gui.text(e),'# API draft')
            combo=wait_for(lambda:u.GetDlgItem(dlg,1148)); buf=c.create_unicode_buffer(str(path))
            inner=wait_for(lambda:u.FindWindowExW(combo,None,'ComboBox',None))
            field=wait_for(lambda:u.FindWindowExW(inner,None,'Edit',None)); wait_for(lambda:u.IsWindowVisible(field) and focused(dlg)==field)
            u.SendMessageW(field,0x0c,0,c.addressof(buf)); self.assertEqual(gui.text(field),str(path))
            u.PostMessageW(u.GetDlgItem(dlg,1),0xf5,0,0)  # click this dialog's native Open button
            wait_for(lambda:'Opened from dialog' in gui.text(e)); wait_for(lambda:'Opened from dialog' in gui.text(p))
            self.assertNotIn('#',gui.text(p)); self.assertNotIn('* ',gui.text(h))

if __name__=='__main__': unittest.main(verbosity=2)
