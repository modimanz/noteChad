#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <wchar.h>
#include <strsafe.h>
#include <string.h>
#include "document.h"
#include "markdown.h"
#include "ai.h"
#include "resource.h"
#include "drag.h"
#include "router.h"

/* Stable heap documents and native EDITs, one GUI thread, many frames. */
typedef struct Tab { Document doc; BOOL wrap; } Tab;
typedef struct Frame {
    struct Frame *next;
    HWND hwnd,strip,footer,findwin;
    HFONT mono,ui;
    Tab **tabs;
    int count,capacity,active;
    BOOL controlled,isolated,hidden,closing,eof_pending;
    unsigned busy;
    FINDREPLACEW fr;
    WCHAR needle[192];
} Frame;
static Frame *frames;
static HINSTANCE instance;
static UINT findmsg;
static BOOL quitting;
static const WCHAR app[]=L"noteChad";
enum { NEW=101, OPEN, SAVE, SAVEAS, EXIT_APP, NEW_TAB, CLOSE_TAB,
       NEW_WINDOW, MOVE_WINDOW, NEXT_TAB, PREV_TAB,
       UNDO_ID=201, CUT_ID, COPY_ID, PASTE_ID, ALL_ID, FIND_ID, NEXT_ID,
       WRAP_ID, TIME_ID, ABOUT=501 };
static const WCHAR filters[]=L"Text and Markdown\0*.txt;*.md;*.markdown\0All files\0*.*\0\0";
static const WCHAR savefilters[]=L"Markdown (*.md)\0*.md\0Text (*.txt)\0*.txt\0All files\0*.*\0\0";
static Tab *current(Frame *f){return f && f->active>=0 && f->active<f->count?f->tabs[f->active]:NULL;}
static Frame *frame_for(HWND h){Frame *f;for(f=frames;f;f=f->next)if(f->hwnd==h)return f;return NULL;}
static void layout(Frame *f);
static void paint_status(Frame *f);
static void select_tab(Frame *f,int index);
static Frame *create_frame(BOOL controlled,BOOL hidden,BOOL blank);
static void open_paths(Frame *f,int count,const WCHAR *const *paths);
static HMENU section(HMENU bar,const WCHAR *name){HMENU m=CreatePopupMenu();AppendMenuW(bar,MF_POPUP,(UINT_PTR)m,name);return m;}
static void item(HMENU m,UINT id,const WCHAR *name){AppendMenuW(m,MF_STRING,id,name);}
static void sep(HMENU m){AppendMenuW(m,MF_SEPARATOR,0,NULL);}
static HMENU menus(BOOL isolated){
    HMENU bar=CreateMenu(),m=section(bar,L"&File");
    item(m,NEW,L"&New tab\tCtrl+N / Ctrl+T");item(m,OPEN,L"&Open…\tCtrl+O");
    item(m,SAVE,L"&Save\tCtrl+S");item(m,SAVEAS,L"Save &as…\tCtrl+Shift+S");
    sep(m);item(m,CLOSE_TAB,L"&Close tab\tCtrl+W");item(m,NEW_WINDOW,L"New &window\tCtrl+Shift+N");
    item(m,MOVE_WINDOW,L"&Move tab to new window\tCtrl+Shift+M");sep(m);item(m,EXIT_APP,L"E&xit window");
    m=section(bar,L"&Edit");item(m,UNDO_ID,L"&Undo\tCtrl+Z");sep(m);item(m,CUT_ID,L"Cu&t\tCtrl+X");item(m,COPY_ID,L"&Copy\tCtrl+C");item(m,PASTE_ID,L"&Paste\tCtrl+V");item(m,ALL_ID,L"Select &all\tCtrl+A");sep(m);item(m,FIND_ID,L"&Find…\tCtrl+F");item(m,NEXT_ID,L"Find &next\tF3");item(m,TIME_ID,L"Time / date\tF5");
    m=section(bar,L"&Markdown");item(m,301,L"&Bold\tCtrl+B");item(m,302,L"Inline &code\tCtrl+`");item(m,303,L"&Task list\tCtrl+Shift+L");sep(m);item(m,304,L"Heading &outline…\tCtrl+Shift+O");item(m,305,L"&Reading preview\tF6");
    m=section(bar,L"&View");item(m,WRAP_ID,L"&Word wrap");item(m,NEXT_TAB,L"Next tab\tCtrl+Tab");item(m,PREV_TAB,L"Previous tab\tCtrl+Shift+Tab");
    m=section(bar,L"&AI");item(m,401,L"&Enable local completion for this session");item(m,402,L"Request a &completion…\tCtrl+Space");
    m=section(bar,L"&Help");item(m,ABOUT,L"&About noteChad");
    if(isolated){UINT ids[]={NEW,CLOSE_TAB,NEW_WINDOW,MOVE_WINDOW,NEXT_TAB,PREV_TAB};for(unsigned i=0;i<ARRAYSIZE(ids);++i)EnableMenuItem(bar,ids[i],MF_BYCOMMAND|MF_GRAYED);}
    return bar;
}
static void tab_label(Tab *t,WCHAR *label,size_t cap){
    const WCHAR *base=wcsrchr(t->doc.path,L'\\');base=base?base+1:t->doc.path;
    StringCchPrintfW(label,cap,L"%s%s",t->doc.dirty?L"* ":L"",*base?base:L"Untitled");
}
static void refresh_tabs(Frame *f){
    if(!f->strip)return;
    /* Avoid DeleteAllItems: status changes must not cancel a live drag. */
    int n=TabCtrl_GetItemCount(f->strip);
    while(n>f->count)TabCtrl_DeleteItem(f->strip,--n);
    for(int i=0;i<f->count;++i){WCHAR label[MAX_PATH+8];TCITEMW ti={0};tab_label(f->tabs[i],label,ARRAYSIZE(label));ti.mask=TCIF_TEXT;ti.pszText=label;
        if(i<n)TabCtrl_SetItem(f->strip,i,&ti);else TabCtrl_InsertItem(f->strip,i,&ti);}
    TabCtrl_SetCurSel(f->strip,f->active);
}
static void paint_status(Frame *f){
    Tab *t=current(f);if(!t || !f->hwnd)return;
    WCHAR title[MAX_PATH+48],label[MAX_PATH+8],state[192];tab_label(t,label,ARRAYSIZE(label));
    StringCchPrintfW(title,ARRAYSIZE(title),L"%s — noteChad",label);SetWindowTextW(f->hwnd,title);
    StringCchPrintfW(state,ARRAYSIZE(state),L"  %s     ·     %s     ·     AI %s%s",tf_format(&t->doc.format),t->doc.dirty?L"Unsaved":L"Ready",ai_enabled()?L"on":L"off",f->controlled?L"     ·     API connected":L"");SetWindowTextW(f->footer,state);
    HMENU m=GetMenu(f->hwnd);CheckMenuItem(m,WRAP_ID,t->wrap?MF_CHECKED:MF_UNCHECKED);CheckMenuItem(m,401,ai_enabled()?MF_CHECKED:MF_UNCHECKED);EnableMenuItem(m,402,ai_enabled()?MF_ENABLED:MF_GRAYED);refresh_tabs(f);
}
static void errorbox(Frame *f,const WCHAR *reason){MessageBoxW(f->hwnd,reason,app,MB_ICONWARNING|MB_OK);}
static BOOL choose(Frame *f,BOOL save,WCHAR path[MAX_PATH]){
    OPENFILENAMEW o={sizeof(o)};o.hwndOwner=f->hwnd;o.lpstrFile=path;o.nMaxFile=MAX_PATH;
    o.lpstrFilter=save?savefilters:filters;o.nFilterIndex=(save && (!*path || (wcsrchr(path,L'.') && !_wcsicmp(wcsrchr(path,L'.'),L".txt"))))?2:1;
    o.lpstrDefExt=save?(o.nFilterIndex==1?L"md":L"txt"):NULL;
    o.Flags=OFN_EXPLORER|OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST|(save?OFN_OVERWRITEPROMPT:OFN_FILEMUSTEXIST);
    return save?GetSaveFileNameW(&o):GetOpenFileNameW(&o);
}
static BOOL save_gui(Frame *f,BOOL as){
    Tab *t=current(f);if(!t)return FALSE;WCHAR path[MAX_PATH],why[512];lstrcpyW(path,t->doc.path);
    if((as||!*path)&&!choose(f,TRUE,path))return FALSE;
    if(!doc_save(&t->doc,path,FALSE,why,ARRAYSIZE(why))){errorbox(f,why);return FALSE;}paint_status(f);return TRUE;
}
static BOOL can_leave(Frame *f){
    Tab *t=current(f);if(!t || !t->doc.dirty)return TRUE;
    WCHAR label[MAX_PATH+8],question[MAX_PATH+128];tab_label(t,label,ARRAYSIZE(label));
    StringCchPrintfW(question,ARRAYSIZE(question),L"Save changes to %s before closing?",label);
    int r=MessageBoxW(f->hwnd,question,app,MB_YESNOCANCEL|MB_ICONQUESTION|MB_DEFBUTTON1);
    return r==IDNO || (r==IDYES && save_gui(f,FALSE));
}
static void dismiss_views(Frame *f,Tab *t){
    if(f->findwin){DestroyWindow(f->findwin);f->findwin=NULL;}
    if(t)md_forget(t->doc.edit);
}
static void layout(Frame *f){
    if(!f || !f->hwnd)return;RECT r;GetClientRect(f->hwnd,&r);
    int height=MulDiv(24,(int)GetDpiForWindow(f->hwnd),96);if(height<20)height=20;
    int foot=r.bottom>height?r.bottom-height:0,top=0;
    if(f->strip){RECT page={0,0,r.right,foot};MoveWindow(f->strip,0,0,r.right,foot,TRUE);TabCtrl_AdjustRect(f->strip,FALSE,&page);top=page.top;
        MoveWindow(f->strip,0,0,r.right,top,TRUE);}
    if(top>foot)top=foot;
    for(int i=0;i<f->count;++i)MoveWindow(f->tabs[i]->doc.edit,0,top,r.right,foot-top,TRUE);
    MoveWindow(f->footer,0,foot,r.right,height,TRUE);
}
static HWND new_edit(Frame *f,BOOL wrapping){
    DWORD style=WS_CHILD|WS_TABSTOP|WS_VSCROLL|ES_MULTILINE|ES_AUTOVSCROLL|ES_NOHIDESEL|ES_WANTRETURN;
    if(!wrapping)style|=WS_HSCROLL|ES_AUTOHSCROLL;
    HWND e=CreateWindowExW(0,L"EDIT",L"",style,0,0,1,1,f->hwnd,(HMENU)900,instance,NULL);
    if(e){SendMessageW(e,WM_SETFONT,(WPARAM)f->mono,TRUE);SendMessageW(e,EM_SETLIMITTEXT,TF_EDIT_LIMIT,0);SendMessageW(e,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,MAKELPARAM(10,10));}
    return e;
}
static BOOL reserve_tabs(Frame *f,int needed){
    if(needed<=f->capacity)return TRUE;
    int cap=f->capacity?f->capacity*2:8;if(cap<needed)cap=needed;
    Tab **p=f->tabs?HeapReAlloc(GetProcessHeap(),0,f->tabs,(SIZE_T)cap*sizeof(*p)):HeapAlloc(GetProcessHeap(),0,(SIZE_T)cap*sizeof(*p));
    if(!p)return FALSE;f->tabs=p;f->capacity=cap;return TRUE;
}
static void select_tab(Frame *f,int index){
    if(index<0 || index>=f->count)return;
    Tab *old=current(f);if(f->active!=index){dismiss_views(f,old);if(old)ShowWindow(old->doc.edit,SW_HIDE);f->active=index;}
    Tab *t=current(f);ShowWindow(t->doc.edit,SW_SHOW);layout(f);paint_status(f);SetFocus(t->doc.edit);
}
static Tab *add_tab(Frame *f){
    if(!reserve_tabs(f,f->count+1))return NULL;
    Tab *t=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*t));if(!t)return NULL;
    HWND e=new_edit(f,TRUE);if(!e){HeapFree(GetProcessHeap(),0,t);return NULL;}
    t->wrap=TRUE;doc_init(&t->doc,e);f->tabs[f->count++]=t;select_tab(f,f->count-1);return t;
}
static void remove_tab(Frame *f,int index,BOOL destroy){
    Tab *t=f->tabs[index];dismiss_views(f,t);
    if(destroy){RemovePropW(t->doc.edit,L"noteChad.ai.request");DestroyWindow(t->doc.edit);HeapFree(GetProcessHeap(),0,t);}
    memmove(f->tabs+index,f->tabs+index+1,(SIZE_T)(f->count-index-1)*sizeof(*f->tabs));--f->count;
    if(f->active>index)--f->active;else if(f->active>=f->count)f->active=f->count-1;
    refresh_tabs(f);if(f->count)select_tab(f,f->active);else f->active=-1;
}
static void open_paths(Frame *f,int count,const WCHAR *const *paths){
    for(int i=0;i<count;++i){
        Tab *t=current(f);BOOL fresh=FALSE;
        if(!t || t->doc.dirty || *t->doc.path || GetWindowTextLengthW(t->doc.edit)){
            if(f->isolated)break;t=add_tab(f);fresh=TRUE;
        }
        if(!t){errorbox(f,L"Not enough memory for another tab.");break;}
        WCHAR why[512];if(!doc_open(&t->doc,paths[i],why,ARRAYSIZE(why))){errorbox(f,why);if(fresh && f->count>1)remove_tab(f,f->active,TRUE);}
        paint_status(f);
    }
}
static void open_gui(Frame *f){
    if(f->isolated){WCHAR path[MAX_PATH]=L"";if(choose(f,FALSE,path)&&can_leave(f)){WCHAR why[512];if(!doc_open(&current(f)->doc,path,why,ARRAYSIZE(why)))errorbox(f,why);paint_status(f);}return;}
    WCHAR *buffer=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,65536*sizeof(WCHAR));if(!buffer)return;
    OPENFILENAMEW o={sizeof(o)};o.hwndOwner=f->hwnd;o.lpstrFile=buffer;o.nMaxFile=65536;o.lpstrFilter=filters;
    o.Flags=OFN_EXPLORER|OFN_ALLOWMULTISELECT|OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST|OFN_FILEMUSTEXIST;
    if(GetOpenFileNameW(&o)){
        const WCHAR *p=buffer+wcslen(buffer)+1;
        if(!*p){const WCHAR *path=buffer;open_paths(f,1,&path);}
        else for(;*p;p+=wcslen(p)+1){WCHAR path[MAX_PATH];if(SUCCEEDED(StringCchPrintfW(path,ARRAYSIZE(path),L"%s\\%s",buffer,p))){const WCHAR *one=path;open_paths(f,1,&one);}else errorbox(f,L"Use a path shorter than 260 characters.");}
    }
    HeapFree(GetProcessHeap(),0,buffer);
}
static void toggle_wrap(Frame *f){
    Tab *t=current(f);HWND old=t->doc.edit;WCHAR *s=doc_snapshot(old);if(!s){errorbox(f,L"Not enough memory to change wrapping.");return;}
    DWORD a=0,b=0;SendMessageW(old,EM_GETSEL,(WPARAM)&a,(LPARAM)&b);HWND e=new_edit(f,!t->wrap);
    if(e && SetWindowTextW(e,s)){dismiss_views(f,t);RemovePropW(old,L"noteChad.ai.request");t->doc.edit=e;++t->doc.revision;t->wrap=!t->wrap;DestroyWindow(old);ShowWindow(e,SW_SHOW);layout(f);SendMessageW(e,EM_SETSEL,a,b);SendMessageW(e,EM_SCROLLCARET,0,0);SetFocus(e);}
    else{if(e)DestroyWindow(e);errorbox(f,L"Could not change wrapping.");}LocalFree(s);paint_status(f);
}
static void find_dialog(Frame *f){
    if(f->findwin){SetForegroundWindow(f->findwin);return;}
    f->fr.lStructSize=sizeof(f->fr);f->fr.hwndOwner=f->hwnd;f->fr.lpstrFindWhat=f->needle;f->fr.wFindWhatLen=ARRAYSIZE(f->needle);
    f->fr.Flags=FR_DOWN|FR_HIDEUPDOWN|FR_HIDEWHOLEWORD|(f->fr.Flags&FR_MATCHCASE);f->findwin=FindTextW(&f->fr);
}
static void find_next(Frame *f){
    Tab *t=current(f);if(!t)return;if(!*f->needle){find_dialog(f);return;}WCHAR *s=doc_snapshot(t->doc.edit);if(!s)return;
    DWORD b=0;SendMessageW(t->doc.edit,EM_GETSEL,0,(LPARAM)&b);int n=(int)wcslen(s),k=(int)wcslen(f->needle),pos=-1;
    DWORD flags=FIND_FROMSTART|((f->fr.Flags&FR_MATCHCASE)?0:LINGUISTIC_IGNORECASE);
    int matched=0;if(b<(DWORD)n){pos=FindNLSStringEx(LOCALE_NAME_USER_DEFAULT,flags,s+b,n-(int)b,f->needle,k,&matched,NULL,NULL,0);if(pos>=0)pos+=(int)b;}
    if(pos<0 && b)pos=FindNLSStringEx(LOCALE_NAME_USER_DEFAULT,flags,s,n,f->needle,k,&matched,NULL,NULL,0);
    if(pos<0)MessageBoxW(f->findwin?f->findwin:f->hwnd,L"No match found.",app,MB_OK|MB_ICONINFORMATION);
    else{SendMessageW(t->doc.edit,EM_SETSEL,pos,pos+matched);SendMessageW(t->doc.edit,EM_SCROLLCARET,0,0);}LocalFree(s);
}
static BOOL movable(Frame *f){return f && !f->isolated && !f->closing && !f->busy && IsWindowEnabled(f->hwnd);}
static BOOL transfer_tab(Frame *source,int index,Frame *target,int at){
    if(!movable(source) || !movable(target) || index<0 || index>=source->count || at<0 || at>target->count)return FALSE;
    Tab *t=source->tabs[index];
    if(source==target){
        if(at>=source->count)at=source->count-1;
        if(at==index)return TRUE;
        Tab *selected=current(source);
        if(at>index)memmove(source->tabs+index,source->tabs+index+1,(SIZE_T)(at-index)*sizeof(*source->tabs));
        else memmove(source->tabs+at+1,source->tabs+at,(SIZE_T)(index-at)*sizeof(*source->tabs));
        source->tabs[at]=t;for(int i=0;i<source->count;++i)if(source->tabs[i]==selected)source->active=i;
        refresh_tabs(source);return TRUE;
    }
    if(!reserve_tabs(target,target->count+1))return FALSE;
    /* Reparent, never serialize/recreate: native undo, text and selection stay. */
    SetLastError(0);HWND prior=SetParent(t->doc.edit,target->hwnd);
    if(!prior && GetLastError())return FALSE;
    dismiss_views(source,t);dismiss_views(target,current(target));
    RemovePropW(t->doc.edit,L"noteChad.ai.request");++t->doc.revision;
    if(current(target))ShowWindow(current(target)->doc.edit,SW_HIDE);
    ShowWindow(t->doc.edit,SW_HIDE);remove_tab(source,index,FALSE);
    memmove(target->tabs+at+1,target->tabs+at,(SIZE_T)(target->count-at)*sizeof(*target->tabs));
    target->tabs[at]=t;++target->count;target->active=at;
    SendMessageW(t->doc.edit,WM_SETFONT,(WPARAM)target->mono,TRUE);select_tab(target,at);
    if(!source->count)DestroyWindow(source->hwnd);
    SetForegroundWindow(target->hwnd);SetFocus(t->doc.edit);return TRUE;
}
static void move_new(Frame *source,int index,const POINT *point){
    if(!movable(source) || index<0 || index>=source->count)return;
    Frame *target=create_frame(FALSE,FALSE,FALSE);if(!target)return;
    if(point)SetWindowPos(target->hwnd,NULL,point->x,point->y,0,0,SWP_NOSIZE|SWP_NOZORDER);
    if(!transfer_tab(source,index,target,0))DestroyWindow(target->hwnd);
}
static void command(Frame *f,UINT id){
    Tab *t=current(f);if(!t)return;
    if(f->isolated && (id==NEW || (id>=NEW_TAB && id<=PREV_TAB)))return;
    switch(id){
    case NEW:case NEW_TAB:if(!add_tab(f))errorbox(f,L"Could not create another tab.");break;
    case NEW_WINDOW:create_frame(FALSE,FALSE,TRUE);break;
    case NEXT_TAB:select_tab(f,(f->active+1)%f->count);break;
    case PREV_TAB:select_tab(f,(f->active+f->count-1)%f->count);break;
    case OPEN:open_gui(f);break;
    case SAVE:save_gui(f,FALSE);break;case SAVEAS:save_gui(f,TRUE);break;
    case EXIT_APP:PostMessageW(f->hwnd,WM_CLOSE,0,0);break;
    case UNDO_ID:SendMessageW(t->doc.edit,WM_UNDO,0,0);break;case CUT_ID:SendMessageW(t->doc.edit,WM_CUT,0,0);break;
    case COPY_ID:SendMessageW(t->doc.edit,WM_COPY,0,0);break;case PASTE_ID:SendMessageW(t->doc.edit,WM_PASTE,0,0);break;
    case ALL_ID:SendMessageW(t->doc.edit,EM_SETSEL,0,-1);break;case FIND_ID:find_dialog(f);break;case NEXT_ID:find_next(f);break;
    case WRAP_ID:toggle_wrap(f);break;case TIME_ID:{WCHAR time[200],date[100];if(GetTimeFormatEx(NULL,TIME_NOSECONDS,NULL,NULL,time,100)&&GetDateFormatEx(NULL,DATE_SHORTDATE,NULL,NULL,date,100,NULL)){StringCchCatW(time,200,L" ");StringCchCatW(time,200,date);SendMessageW(t->doc.edit,EM_REPLACESEL,TRUE,(LPARAM)time);}break;}
    case 401:ai_toggle(f->hwnd);for(Frame *p=frames;p;p=p->next)paint_status(p);break;
    case 402:if(ai_enabled())ai_request(f->hwnd,t->doc.edit,t->doc.revision);break;
    case ABOUT:MessageBoxW(f->hwnd,L"noteChad\nNative text and Markdown editor\n\nCtrl+T: New tab • Ctrl+W: Close tab\nCtrl+Shift+N: New window\nCtrl+Shift+M: Move tab to new window\nF6: Reading preview\nCtrl+Shift+O: Heading outline\nCtrl+Space: Local AI completion (opt-in)\n\nHermes can open an isolated session with --api or --api-window.",app,MB_OK|MB_ICONINFORMATION);break;
    default:if(id>=301 && id<=305)md_command(f->hwnd,t->doc.edit,id);break;
    }
}
static BOOL can_close_frame(Frame *f){
    int selected=f->active;
    /* Preflight without destroying ANY tabs. Later Cancel retains earlier
       'Don't save' drafts, undo buffers, dirty flags and disk origins. */
    for(int i=0;i<f->count;++i)if(f->tabs[i]->doc.dirty){select_tab(f,i);if(!can_leave(f)){select_tab(f,selected);return FALSE;}}
    return TRUE;
}
static void finish_busy(Frame *f){
    --f->busy;
    if(!f->busy && f->eof_pending)PostMessageW(f->hwnd,WM_NOTECHAD_EOF,0,0);
}
static LRESULT CALLBACK window_proc(HWND h,UINT m,WPARAM w,LPARAM l){
    Frame *f=(Frame *)GetWindowLongPtrW(h,GWLP_USERDATA);
    if(m==WM_NCCREATE){f=(Frame *)((CREATESTRUCTW *)l)->lpCreateParams;f->hwnd=h;SetWindowLongPtrW(h,GWLP_USERDATA,(LONG_PTR)f);}
    if(!f)return DefWindowProcW(h,m,w,l);
    if(findmsg && m==findmsg && (FINDREPLACEW *)l==&f->fr){if(f->fr.Flags&FR_DIALOGTERM)f->findwin=NULL;else if(f->fr.Flags&FR_FINDNEXT)find_next(f);return 0;}
    switch(m){
    case WM_CREATE:{int dpi=(int)GetDpiForWindow(h);
        f->mono=CreateFontW(-MulDiv(12,dpi,72),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,FIXED_PITCH,L"Consolas");
        f->ui=CreateFontW(-MulDiv(9,dpi,72),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        f->footer=CreateWindowExW(0,L"STATIC",NULL,WS_CHILD|WS_VISIBLE|SS_CENTERIMAGE|SS_NOPREFIX,0,0,1,1,h,NULL,instance,NULL);
        if(!f->footer)return -1;SendMessageW(f->footer,WM_SETFONT,(WPARAM)f->ui,TRUE);
        if(!f->isolated){f->strip=CreateWindowExW(0,WC_TABCONTROLW,L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|TCS_SINGLELINE|TCS_FOCUSNEVER,0,0,1,1,h,(HMENU)901,instance,NULL);
            if(!f->strip)return -1;SendMessageW(f->strip,WM_SETFONT,(WPARAM)f->ui,TRUE);}
        return 0;}
    case WM_SIZE:layout(f);return 0;
    case WM_SETFOCUS:if(current(f))SetFocus(current(f)->doc.edit);return 0;
    case WM_NOTIFY:if(f->strip && ((NMHDR *)l)->hwndFrom==f->strip && ((NMHDR *)l)->code==TCN_SELCHANGE){if(!f->busy)select_tab(f,TabCtrl_GetCurSel(f->strip));else refresh_tabs(f);}return 0;
    case WM_COMMAND:
        if(l){for(int i=0;i<f->count;++i)if((HWND)l==f->tabs[i]->doc.edit){if(HIWORD(w)==EN_CHANGE){doc_changed(&f->tabs[i]->doc);paint_status(f);}return 0;}return 0;}
        if(f->busy || !IsWindowEnabled(h))return 0;
        /* These calls may destroy this frame: do not touch f afterward. */
        if(LOWORD(w)==MOVE_WINDOW){if(!f->isolated)move_new(f,f->active,NULL);return 0;}
        if(LOWORD(w)==CLOSE_TAB && !f->isolated){++f->busy;BOOL yes=current(f)&&can_leave(f);--f->busy;if(yes){if(f->count==1)DestroyWindow(h);else remove_tab(f,f->active,TRUE);}return 0;}
        ++f->busy;command(f,LOWORD(w));finish_busy(f);return 0;
    case WM_NC_TABDROP:{TabDrop drop;
        if(!drag_take(h,l,&drop))return 0;
        Frame *source=frame_for(h),*target=drop.target?frame_for(drop.target):NULL;
        if(!movable(source) || w>=(WPARAM)source->count)return 0;
        if(drop.target){if(movable(target))transfer_tab(source,(int)w,target,drop.index);}
        else move_new(source,(int)w,&drop.screen);return 0;}
    case WM_DROPFILES:{HDROP drop=(HDROP)w;
        if(!f->isolated && !f->busy && IsWindowEnabled(h)){UINT n=DragQueryFileW(drop,0xFFFFFFFF,NULL,0);++f->busy;
            for(UINT i=0;i<n;++i){UINT len=DragQueryFileW(drop,i,NULL,0);WCHAR path[MAX_PATH];if(len && len<MAX_PATH && DragQueryFileW(drop,i,path,MAX_PATH)){const WCHAR *p=path;open_paths(f,1,&p);}else errorbox(f,L"Use a path shorter than 260 characters.");}--f->busy;}
        DragFinish(drop);return 0;}
    case WM_NOTECHAD_API:if(f->controlled && current(f) && api_owns(l)){ApiRequest *r=(ApiRequest *)l;doc_dispatch(&current(f)->doc,r,!f->busy&&IsWindowEnabled(h));if(r->ok && strcmp(r->verb,"read"))paint_status(f);}return 0;
    case WM_NOTECHAD_EOF:if(f->controlled){if(f->hidden && f->busy){f->eof_pending=TRUE;return 0;}f->eof_pending=FALSE;f->controlled=FALSE;if(f->hidden)DestroyWindow(h);else paint_status(f);}return 0;
    case WM_NOTECHAD_AI:{Tab *t=current(f);++f->busy;ai_result(h,t?t->doc.edit:NULL,t?t->doc.revision:0,l);finish_busy(f);return 0;}
    case WM_QUERYENDSESSION:case WM_CLOSE:{if(f->busy || f->closing)return FALSE;++f->busy;BOOL yes=can_close_frame(f);finish_busy(f);if(m==WM_QUERYENDSESSION)return yes;if(yes)DestroyWindow(h);return 0;}
    case WM_DESTROY:{f->closing=TRUE;dismiss_views(f,current(f));
        for(int i=0;i<f->count;++i){md_forget(f->tabs[i]->doc.edit);RemovePropW(f->tabs[i]->doc.edit,L"noteChad.ai.request");}return 0;}
    case WM_NCDESTROY:{Frame **p=&frames;while(*p && *p!=f)p=&(*p)->next;if(*p)*p=f->next;
        for(int i=0;i<f->count;++i)HeapFree(GetProcessHeap(),0,f->tabs[i]);
        if(f->tabs)HeapFree(GetProcessHeap(),0,f->tabs);DeleteObject(f->mono);DeleteObject(f->ui);
        SetWindowLongPtrW(h,GWLP_USERDATA,0);HeapFree(GetProcessHeap(),0,f);
        if(!frames && !quitting){quitting=TRUE;PostQuitMessage(0);}return DefWindowProcW(h,m,w,l);}
    }
    return DefWindowProcW(h,m,w,l);
}
static BOOL present_frame(Frame *f){
    HWND h=f->hwnd;f->hidden=FALSE;
    if(!f->isolated)DragAcceptFiles(h,TRUE);ShowWindow(h,SW_SHOW);UpdateWindow(h);
    /* drag_attach validates an ordinary visible frame. */
    if(!f->isolated && !drag_attach(f->strip)){DestroyWindow(h);return FALSE;}
    if(current(f))SetFocus(current(f)->doc.edit);return TRUE;
}
static Frame *create_frame(BOOL controlled,BOOL hidden,BOOL blank){
    Frame *f=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*f));if(!f)return NULL;
    f->controlled=controlled;f->isolated=controlled;f->hidden=hidden;f->active=-1;f->next=frames;frames=f;
    HWND h=CreateWindowW(L"NoteChad.Editor",app,WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,900,650,NULL,menus(controlled),instance,f);
    if(!h)return NULL;
    if(blank && !add_tab(f)){DestroyWindow(h);return NULL;}
    if(!hidden && !present_frame(f))return NULL;
    return f;
}
static void routed_window(int count,const WCHAR *const *paths){
    if(quitting)return;Frame *f=create_frame(FALSE,TRUE,TRUE);if(f){++f->busy;open_paths(f,count,paths);--f->busy;if(present_frame(f))SetForegroundWindow(f->hwnd);}
}
#define C (FVIRTKEY|FCONTROL)
#define CS (FVIRTKEY|FCONTROL|FSHIFT)
int WINAPI wWinMain(HINSTANCE inst,HINSTANCE prev,PWSTR cmd,int show){
    (void)prev;(void)cmd;(void)show;instance=inst;int argc=0;WCHAR **argv=CommandLineToArgvW(GetCommandLineW(),&argc);if(!argv)return 2;
    BOOL standalone=FALSE;int args=1;
    for(int i=1;i<argc;++i)if(!wcscmp(argv[i],L"--standalone"))standalone=TRUE;else argv[args++]=argv[i];argc=args;argv[argc]=NULL;
    BOOL controlled=argc==2 && (!wcscmp(argv[1],L"--api") || !wcscmp(argv[1],L"--api-window"));
    BOOL hidden=controlled && !wcscmp(argv[1],L"--api");
    SetProcessDPIAware();INITCOMMONCONTROLSEX common={sizeof(common),ICC_TAB_CLASSES};InitCommonControlsEx(&common);
    WNDCLASSW wc={0};wc.lpfnWndProc=window_proc;wc.hInstance=instance;wc.lpszClassName=L"NoteChad.Editor";wc.hCursor=LoadCursorW(NULL,IDC_ARROW);wc.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(IDI_NOTECHAD));wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
    if(!RegisterClassW(&wc)){LocalFree(argv);return 1;}findmsg=RegisterWindowMessageW(FINDMSGSTRINGW);
    BOOL routing=!controlled && !standalone;
    if(routing){int result=router_start(argc,argv,routed_window);if(result!=ROUTER_PRIMARY){LocalFree(argv);return result==ROUTER_FORWARDED?0:1;}}
    Frame *f=create_frame(controlled,hidden || !controlled,TRUE);if(!f){if(routing)router_stop();LocalFree(argv);return 1;}
    if(!controlled){++f->busy;open_paths(f,argc-1,(const WCHAR *const *)(argv+1));--f->busy;if(!present_frame(f)){if(routing)router_stop();LocalFree(argv);return 1;}}LocalFree(argv);
    if(controlled && !api_start(f->hwnd)){DestroyWindow(f->hwnd);return 2;}
    ACCEL keys[]={ {C,'N',NEW},{C,'T',NEW_TAB},{C,'W',CLOSE_TAB},{CS,'N',NEW_WINDOW},{CS,'M',MOVE_WINDOW},{C,VK_TAB,NEXT_TAB},{CS,VK_TAB,PREV_TAB},
        {C,'O',OPEN},{C,'S',SAVE},{CS,'S',SAVEAS},{C,'A',ALL_ID},{C,'F',FIND_ID},{FVIRTKEY,VK_F3,NEXT_ID},{FVIRTKEY,VK_F5,TIME_ID},
        {C,'B',301},{C,VK_OEM_3,302},{CS,'L',303},{CS,'O',304},{FVIRTKEY,VK_F6,305},{C,VK_SPACE,402} };
    HACCEL accelerators=CreateAcceleratorTableW(keys,ARRAYSIZE(keys));MSG msg;int res;
    while((res=GetMessageW(&msg,NULL,0,0))>0){
        BOOL dialog=FALSE;for(Frame *p=frames;p;p=p->next)if(p->findwin && IsDialogMessageW(p->findwin,&msg)){dialog=TRUE;break;}if(dialog)continue;
        Frame *owner=frame_for(GetAncestor(msg.hwnd,GA_ROOT));
        if(owner && TranslateAcceleratorW(owner->hwnd,accelerators,&msg))continue;
        TranslateMessage(&msg);DispatchMessageW(&msg);
    }
    DestroyAcceleratorTable(accelerators);if(routing)router_stop();return res<0?1:0;
}
