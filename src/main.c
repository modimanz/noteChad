#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <wchar.h>
#include <strsafe.h>
#include <string.h>
#include "document.h"
#include "markdown.h"
#include "ai.h"
#include "resource.h"

/* One top-level window, one native text control, no service or polling loop. */
static Document doc;
static HWND mainwin,footer,findwin;
static HFONT mono,ui;
static BOOL controlled,hidden,wrap=TRUE;
static unsigned busy;
static FINDREPLACEW fr;
static WCHAR needle[192];
static UINT findmsg;
static const WCHAR app[]=L"nodeChad";
enum { NEW=101, OPEN, SAVE, SAVEAS, EXIT_APP, UNDO_ID=201, CUT_ID, COPY_ID,
       PASTE_ID, ALL_ID, FIND_ID, NEXT_ID, WRAP_ID, TIME_ID, ABOUT=501 };
static const WCHAR filters[]=L"Text and Markdown\0*.txt;*.md;*.markdown\0All files\0*.*\0\0";
static const WCHAR savefilters[]=L"Markdown (*.md)\0*.md\0Text (*.txt)\0*.txt\0All files\0*.*\0\0";
static HMENU section(HMENU bar,const WCHAR *name){ HMENU m=CreatePopupMenu();AppendMenuW(bar,MF_POPUP,(UINT_PTR)m,name);return m; }
static void item(HMENU m,UINT id,const WCHAR *name){AppendMenuW(m,MF_STRING,id,name);}
static void sep(HMENU m){AppendMenuW(m,MF_SEPARATOR,0,NULL);}
static HMENU menus(void){
    HMENU bar=CreateMenu(),m=section(bar,L"&File");
    item(m,NEW,L"&New\tCtrl+N");item(m,OPEN,L"&Open…\tCtrl+O");item(m,SAVE,L"&Save\tCtrl+S");item(m,SAVEAS,L"Save &as…\tCtrl+Shift+S");sep(m);item(m,EXIT_APP,L"E&xit");
    m=section(bar,L"&Edit");item(m,UNDO_ID,L"&Undo\tCtrl+Z");sep(m);item(m,CUT_ID,L"Cu&t\tCtrl+X");item(m,COPY_ID,L"&Copy\tCtrl+C");item(m,PASTE_ID,L"&Paste\tCtrl+V");item(m,ALL_ID,L"Select &all\tCtrl+A");sep(m);item(m,FIND_ID,L"&Find…\tCtrl+F");item(m,NEXT_ID,L"Find &next\tF3");item(m,TIME_ID,L"Time / date\tF5");
    m=section(bar,L"&Markdown");item(m,301,L"&Bold\tCtrl+B");item(m,302,L"Inline &code\tCtrl+`");item(m,303,L"&Task list\tCtrl+Shift+L");sep(m);item(m,304,L"Heading &outline…\tCtrl+Shift+O");item(m,305,L"&Reading preview\tF6");
    m=section(bar,L"&View");item(m,WRAP_ID,L"&Word wrap");
    m=section(bar,L"&AI");item(m,401,L"&Enable local completion for this session");item(m,402,L"Request a &completion…\tCtrl+Space");
    m=section(bar,L"&Help");item(m,ABOUT,L"&About nodeChad");return bar;
}
static void paint_status(void){
    if(!doc.edit||!mainwin)return;
    WCHAR title[MAX_PATH+48],state[192]; const WCHAR *base=wcsrchr(doc.path,'\\');base=base?base+1:doc.path;
    StringCchPrintfW(title,ARRAYSIZE(title),L"%s%s — nodeChad",doc.dirty?L"* ":L"",*base?base:L"Untitled"); SetWindowTextW(mainwin,title);
    StringCchPrintfW(state,ARRAYSIZE(state),L"  %s     ·     %s     ·     AI %s%s",tf_format(&doc.format),doc.dirty?L"Unsaved":L"Ready",ai_enabled()?L"on":L"off",controlled?L"     ·     API connected":L"");SetWindowTextW(footer,state);
    HMENU m=GetMenu(mainwin);CheckMenuItem(m,WRAP_ID,wrap?MF_CHECKED:MF_UNCHECKED);CheckMenuItem(m,401,ai_enabled()?MF_CHECKED:MF_UNCHECKED);EnableMenuItem(m,402,ai_enabled()?MF_ENABLED:MF_GRAYED);
}
static void errorbox(const WCHAR *reason){ MessageBoxW(mainwin,reason,app,MB_ICONWARNING|MB_OK); }
static BOOL choose(BOOL save,WCHAR path[MAX_PATH]){
    OPENFILENAMEW f={sizeof(f)};f.hwndOwner=mainwin;f.lpstrFile=path;f.nMaxFile=MAX_PATH;
    f.lpstrFilter=save?savefilters:filters;f.nFilterIndex=(save && (!*path || (wcsrchr(path,'.') && !_wcsicmp(wcsrchr(path,'.'),L".txt"))))?2:1;
    f.lpstrDefExt=save?(f.nFilterIndex==1?L"md":L"txt"):NULL;
    f.Flags=OFN_EXPLORER|OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST|(save?OFN_OVERWRITEPROMPT:OFN_FILEMUSTEXIST);
    return save?GetSaveFileNameW(&f):GetOpenFileNameW(&f);
}
static BOOL save_gui(BOOL as){
    WCHAR path[MAX_PATH],why[512]; lstrcpyW(path,doc.path);
    if((as||!*path)&&!choose(TRUE,path))return FALSE;
    if(!doc_save(&doc,path,FALSE,why,ARRAYSIZE(why))){errorbox(why);return FALSE;}paint_status();return TRUE;
}
static BOOL can_leave(void){
    if(!doc.dirty)return TRUE;
    int r=MessageBoxW(mainwin,L"Save your changes before continuing?",app,MB_YESNOCANCEL|MB_ICONQUESTION|MB_DEFBUTTON1);
    return r==IDNO || (r==IDYES && save_gui(FALSE));
}
static void open_gui(const WCHAR *path){ WCHAR why[512]; if(!doc_open(&doc,path,why,ARRAYSIZE(why)))errorbox(why);paint_status(); }
static void layout(HWND host){RECT r;GetClientRect(host,&r); int height=MulDiv(24,(int)GetDpiForWindow(host),96);if(height<20)height=20;
    int foot=r.bottom>height?r.bottom-height:0; MoveWindow(doc.edit,0,0,r.right,foot,TRUE);MoveWindow(footer,0,foot,r.right,height,TRUE);}
static HWND new_edit(HWND host,BOOL wrapping){
    DWORD style=WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_VSCROLL|ES_MULTILINE|ES_AUTOVSCROLL|ES_NOHIDESEL|ES_WANTRETURN;
    if(!wrapping)style|=WS_HSCROLL|ES_AUTOHSCROLL;
    HWND e=CreateWindowExW(0,L"EDIT",L"",style,0,0,1,1,host,(HMENU)900,GetModuleHandleW(NULL),NULL);
    if(e){SendMessageW(e,WM_SETFONT,(WPARAM)mono,TRUE); SendMessageW(e,EM_SETLIMITTEXT,TF_EDIT_LIMIT,0);SendMessageW(e,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,MAKELPARAM(10,10));}
    return e;
}
static void toggle_wrap(void){
    HWND old=doc.edit; WCHAR *s=doc_snapshot(old); if(!s){errorbox(L"Not enough memory to change wrapping.");return;}
    DWORD a=0,b=0;SendMessageW(old,EM_GETSEL,(WPARAM)&a,(LPARAM)&b); HWND e=new_edit(mainwin,!wrap);
    if(e && SetWindowTextW(e,s)){doc.edit=e;wrap=!wrap;DestroyWindow(old);layout(mainwin);SendMessageW(e,EM_SETSEL,a,b);SendMessageW(e,EM_SCROLLCARET,0,0);SetFocus(e);}
    else {if(e)DestroyWindow(e);errorbox(L"Could not change wrapping.");} LocalFree(s);paint_status();
}
static void find_dialog(void){
    if(findwin){SetForegroundWindow(findwin);return;}
    fr.lStructSize=sizeof(fr);fr.hwndOwner=mainwin;fr.lpstrFindWhat=needle;fr.wFindWhatLen=ARRAYSIZE(needle);
    fr.Flags=FR_DOWN|FR_HIDEUPDOWN|FR_HIDEWHOLEWORD|(fr.Flags&FR_MATCHCASE);findwin=FindTextW(&fr);
}
static void find_next(void){
    if(!*needle){find_dialog();return;} WCHAR *s=doc_snapshot(doc.edit);if(!s)return;
    DWORD b=0;SendMessageW(doc.edit,EM_GETSEL,0,(LPARAM)&b);int n=(int)wcslen(s),k=(int)wcslen(needle),pos=-1;
    DWORD flags=FIND_FROMSTART|((fr.Flags&FR_MATCHCASE)?0:LINGUISTIC_IGNORECASE);
    int matched=0; if(b<(DWORD)n){pos=FindNLSStringEx(LOCALE_NAME_USER_DEFAULT,flags,s+b,n-(int)b,needle,k,&matched,NULL,NULL,0);if(pos>=0)pos+=(int)b;}
    if(pos<0 && b)pos=FindNLSStringEx(LOCALE_NAME_USER_DEFAULT,flags,s,n,needle,k,&matched,NULL,NULL,0);
    if(pos<0)MessageBoxW(findwin?findwin:mainwin,L"No match found.",app,MB_OK|MB_ICONINFORMATION);
    else{SendMessageW(doc.edit,EM_SETSEL,pos,pos+matched);SendMessageW(doc.edit,EM_SCROLLCARET,0,0);}LocalFree(s);
}
static void command(UINT id){
    switch(id){
    case NEW: if(can_leave()){doc_new(&doc);paint_status();}break;
    case OPEN: {WCHAR path[MAX_PATH]=L"";if(choose(FALSE,path)&&can_leave())open_gui(path);break;}
    case SAVE:save_gui(FALSE);break;case SAVEAS:save_gui(TRUE);break;case EXIT_APP:PostMessageW(mainwin,WM_CLOSE,0,0);break;
    case UNDO_ID:SendMessageW(doc.edit,WM_UNDO,0,0);break;case CUT_ID:SendMessageW(doc.edit,WM_CUT,0,0);break;
    case COPY_ID:SendMessageW(doc.edit,WM_COPY,0,0);break;case PASTE_ID:SendMessageW(doc.edit,WM_PASTE,0,0);break;
    case ALL_ID:SendMessageW(doc.edit,EM_SETSEL,0,-1);break;case FIND_ID:find_dialog();break;case NEXT_ID:find_next();break;
    case WRAP_ID:toggle_wrap();break;case TIME_ID:{WCHAR t[200],dt[100];if(GetTimeFormatEx(NULL,TIME_NOSECONDS,NULL,NULL,t,100)&&GetDateFormatEx(NULL,DATE_SHORTDATE,NULL,NULL,dt,100,NULL)){StringCchCatW(t,200,L" ");StringCchCatW(t,200,dt);SendMessageW(doc.edit,EM_REPLACESEL,TRUE,(LPARAM)t);}break;}
    case 401:ai_toggle(mainwin);paint_status();break;case 402:if(ai_enabled())ai_request(mainwin,doc.edit,doc.revision);break;
    case ABOUT:MessageBoxW(mainwin,L"nodeChad\nNative text and Markdown editor\n\nF6: Reading preview\nCtrl+Shift+O: Heading outline\nCtrl+Space: Request a local AI completion (opt-in)\n\nHermes can open a controlled session with --api or --api-window.",app,MB_OK|MB_ICONINFORMATION);break;
    default:if(id>=301 && id<=305) md_command(mainwin,doc.edit,id);break;
    }
}
static LRESULT CALLBACK window_proc(HWND h,UINT m,WPARAM w,LPARAM l){
    if(findmsg && m==findmsg && (FINDREPLACEW *)l==&fr){if(fr.Flags&FR_DIALOGTERM)findwin=NULL;else if(fr.Flags&FR_FINDNEXT)find_next();return 0;}
    switch(m){
    case WM_CREATE:{mainwin=h;int dpi=(int)GetDpiForWindow(h);mono=CreateFontW(-MulDiv(12,dpi,72),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,FIXED_PITCH,L"Consolas");
        ui=CreateFontW(-MulDiv(9,dpi,72),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        HWND e=new_edit(h,TRUE);footer=CreateWindowExW(0,L"STATIC",NULL,WS_CHILD|WS_VISIBLE|SS_CENTERIMAGE|SS_NOPREFIX,0,0,1,1,h,NULL,NULL,NULL);
        if(!e||!footer)return -1;doc_init(&doc,e);SendMessageW(footer,WM_SETFONT,(WPARAM)ui,TRUE);paint_status();return 0;}
    case WM_SIZE:layout(h);return 0;case WM_SETFOCUS:SetFocus(doc.edit);return 0;
    case WM_COMMAND:if((HWND)l==doc.edit && doc.edit){if(HIWORD(w)==EN_CHANGE){BOOL first=!doc.dirty;doc_changed(&doc);if(first)paint_status();}return 0;}
        if(!l){++busy;command(LOWORD(w));--busy;}return 0;
    case WM_DROPFILES:{HDROP drop=(HDROP)w;UINT n=DragQueryFileW(drop,0,NULL,0);WCHAR path[MAX_PATH];++busy;if(n && n<MAX_PATH && DragQueryFileW(drop,0,path,MAX_PATH) && can_leave())open_gui(path);--busy;DragFinish(drop);return 0;}
    case WM_NODECHAD_API:if(controlled&&api_owns(l)){ApiRequest *r=(ApiRequest *)l;doc_dispatch(&doc,r,!busy&&IsWindowEnabled(h));if(r->ok && strcmp(r->verb,"read"))paint_status();}return 0;
    case WM_NODECHAD_EOF:if(controlled){controlled=FALSE;if(hidden)DestroyWindow(h);else paint_status();}return 0;
    case WM_NODECHAD_AI:++busy;ai_result(h,doc.edit,doc.revision,l);--busy;return 0;
    case WM_QUERYENDSESSION:case WM_CLOSE:{++busy;BOOL yes=can_leave();--busy;if(m==WM_QUERYENDSESSION)return yes;if(yes)DestroyWindow(h);return 0;}
    case WM_DESTROY:DeleteObject(mono);DeleteObject(ui);PostQuitMessage(0);return 0;
    }return DefWindowProcW(h,m,w,l);
}
#define C (FVIRTKEY|FCONTROL)
#define CS (FVIRTKEY|FCONTROL|FSHIFT)
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE prev,PWSTR cmd,int show){
    (void)prev;(void)cmd;int argc=0; WCHAR **argv=CommandLineToArgvW(GetCommandLineW(),&argc); if(!argv)return 2;
    controlled=argc==2 && (!wcscmp(argv[1],L"--api") || !wcscmp(argv[1],L"--api-window")); hidden=controlled && !wcscmp(argv[1],L"--api");
    SetProcessDPIAware(); WNDCLASSW wc={0};wc.lpfnWndProc=window_proc;wc.hInstance=instance;wc.lpszClassName=L"NodeChad.Editor";wc.hCursor=LoadCursorW(NULL,IDC_ARROW);wc.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(IDI_NODECHAD));wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
    if(!RegisterClassW(&wc))return 1;findmsg=RegisterWindowMessageW(FINDMSGSTRINGW);
    HWND h=CreateWindowW(wc.lpszClassName,app,WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,900,650,NULL,menus(),instance,NULL);if(!h)return 1;
    if(!controlled && argc>=2)open_gui(argv[1]);LocalFree(argv);
    if(controlled && !api_start(h)){DestroyWindow(h);return 2;}
    if(!hidden){DragAcceptFiles(h,TRUE);ShowWindow(h,show);UpdateWindow(h);SetFocus(doc.edit);}
    ACCEL keys[]={ {C,'N',NEW},{C,'O',OPEN},{C,'S',SAVE},{CS,'S',SAVEAS},{C,'A',ALL_ID},{C,'F',FIND_ID},{FVIRTKEY,VK_F3,NEXT_ID},{FVIRTKEY,VK_F5,TIME_ID},
        {C,'B',301},{C,VK_OEM_3,302},{CS,'L',303},{CS,'O',304},{FVIRTKEY,VK_F6,305},{C,VK_SPACE,402} };
    HACCEL accelerators=CreateAcceleratorTableW(keys,ARRAYSIZE(keys));MSG msg;int res;
    while((res=GetMessageW(&msg,NULL,0,0))>0){if(findwin && IsDialogMessageW(findwin,&msg))continue;
        if((msg.hwnd==h||IsChild(h,msg.hwnd)) && TranslateAcceleratorW(h,accelerators,&msg))continue;
        TranslateMessage(&msg);DispatchMessageW(&msg);}
    DestroyAcceleratorTable(accelerators);return res<0?1:0;
}
