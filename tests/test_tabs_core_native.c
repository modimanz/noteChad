/* White-box GUI-thread boundary checks using the real application/modules. */
#define wWinMain tabs_application_main
#include "../src/main.c"
#undef wWinMain
#include <stdio.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;} } while(0)
static HWND prompt_owner;
static BOOL cancelled;
static void CALLBACK cancel_close(HWND unused,UINT msg,UINT_PTR id,DWORD now){
    (void)unused;(void)msg;(void)now;
    HWND dlg=GetLastActivePopup(prompt_owner);DWORD pid=0;GetWindowThreadProcessId(dlg,&pid);
    if(pid==GetCurrentProcessId() && GetWindow(dlg,GW_OWNER)==prompt_owner && GetDlgItem(dlg,IDYES) && GetDlgItem(dlg,IDCANCEL)){
        KillTimer(NULL,id);cancelled=TRUE;PostMessageW(dlg,WM_COMMAND,IDCANCEL,0);
    }
}
int main(void){
    instance=GetModuleHandleW(NULL);SetProcessDPIAware();
    INITCOMMONCONTROLSEX common={sizeof(common),ICC_TAB_CLASSES};InitCommonControlsEx(&common);
    WNDCLASSW wc={0};wc.lpfnWndProc=window_proc;wc.hInstance=instance;wc.lpszClassName=L"NoteChad.Editor";
    wc.hCursor=LoadCursorW(NULL,IDC_ARROW);CHECK(RegisterClassW(&wc));
    findmsg=RegisterWindowMessageW(FINDMSGSTRINGW);
    Frame *a=create_frame(FALSE,FALSE,TRUE),*b=create_frame(FALSE,FALSE,TRUE),*api=create_frame(TRUE,TRUE,TRUE);
    CHECK(a && b && api);
    CHECK(GetFocus()!=current(api)->doc.edit); /* Headless automation must not receive desktop typing. */
    Tab *one=current(a),*two=add_tab(a),*three=add_tab(a),*dest=current(b);
    CHECK(one && two && three && dest);
    CHECK(SetWindowTextW(two->doc.edit,L"preserved"));two->doc.dirty=TRUE;
    lstrcpyW(two->doc.path,L"C:\\synthetic-origin.md");
    RECT close_rect;CHECK(drag_close_rect(a->strip,1,&close_rect));
    LPARAM click=MAKELPARAM((close_rect.left+close_rect.right)/2,(close_rect.top+close_rect.bottom)/2);
    prompt_owner=a->hwnd;CHECK(SetTimer(NULL,0,40,cancel_close));
    SendMessageW(a->strip,WM_LBUTTONDOWN,MK_LBUTTON,click);SendMessageW(a->strip,WM_LBUTTONUP,0,click);
    CHECK(cancelled && current(a)==three && a->count==3 && a->tabs[1]==two && two->doc.dirty);
    DWORD low=0,high=0;SendMessageW(two->doc.edit,EM_SETSEL,2,6);
    CHECK(transfer_tab(a,0,a,2));CHECK(a->tabs[0]==two && a->tabs[1]==three && a->tabs[2]==one);
    CHECK(current(a)==three);
    CHECK(transfer_tab(a,2,a,0));CHECK(a->tabs[0]==one && a->tabs[1]==two && a->tabs[2]==three);
    CHECK(!transfer_tab(a,-1,b,0));CHECK(!transfer_tab(a,0,b,2));
    ++b->busy;CHECK(!transfer_tab(a,1,b,0));--b->busy;
    ++a->busy;CHECK(!transfer_tab(a,1,b,0));--a->busy;
    CHECK(!transfer_tab(a,1,api,0));CHECK(!transfer_tab(api,0,a,0));
    SendMessageW(a->hwnd,WM_NC_TABDROP,1,(LPARAM)1);CHECK(a->count==3 && b->count==1);
    HWND native=two->doc.edit;unsigned long long rev=two->doc.revision;
    CHECK(transfer_tab(a,1,b,0));CHECK(b->count==2 && a->count==2);
    CHECK(current(b)==two && b->tabs[1]==dest && GetParent(native)==b->hwnd);
    CHECK(two->doc.edit==native && two->doc.dirty && !wcscmp(two->doc.path,L"C:\\synthetic-origin.md"));
    CHECK(two->doc.revision>rev);SendMessageW(native,EM_GETSEL,(WPARAM)&low,(LPARAM)&high);CHECK(low==2 && high==6);
    CHECK(transfer_tab(b,0,a,a->count));CHECK(a->tabs[2]==two && current(a)==two);
    HWND source_window=b->hwnd;
    CHECK(transfer_tab(b,0,a,1));CHECK(!IsWindow(source_window));
    CHECK(a->count==4 && a->tabs[1]==dest);
    select_tab(a,2); /* Heading/find commands remain bound to this frame/tab. */
    CHECK(SetWindowTextW(current(a)->doc.edit,L"before needle after"));
    lstrcpyW(a->needle,L"needle");SendMessageW(current(a)->doc.edit,EM_SETSEL,0,0);find_next(a);
    SendMessageW(current(a)->doc.edit,EM_GETSEL,(WPARAM)&low,(LPARAM)&high);CHECK(low==7 && high==13);
    find_dialog(a);CHECK(IsWindow(a->findwin));select_tab(a,0);CHECK(!a->findwin);
    /* Real HDROP batch, allocated in this isolated fixture process/cwd. */
    const WCHAR names[]=L"drop-one.md\0drop-two.md\0";
    const WCHAR *nameset[2]={L"drop-one.md",L"drop-two.md"};
    for(int i=0;i<2;++i){HANDLE file=CreateFileW(nameset[i],GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);DWORD written;CHECK(file!=INVALID_HANDLE_VALUE);CHECK(WriteFile(file,"data",4,&written,NULL));CloseHandle(file);}
    struct DropHeader {DWORD offset;POINT pt;BOOL nonclient,wide;};
    HGLOBAL drop=GlobalAlloc(GHND,sizeof(struct DropHeader)+sizeof(names));CHECK(drop);
    struct DropHeader *header=GlobalLock(drop);CHECK(header);header->offset=sizeof(*header);header->wide=TRUE;
    memcpy((BYTE *)header+sizeof(*header),names,sizeof(names));GlobalUnlock(drop);
    Frame *d=create_frame(FALSE,FALSE,TRUE);CHECK(d);SendMessageW(d->hwnd,WM_DROPFILES,(WPARAM)drop,0);
    CHECK(d->count==2 && wcsstr(d->tabs[0]->doc.path,L"drop-one.md") && wcsstr(d->tabs[1]->doc.path,L"drop-two.md"));
    DestroyWindow(d->hwnd);CHECK(DeleteFileW(nameset[0]) && DeleteFileW(nameset[1]));
    DestroyWindow(api->hwnd);DestroyWindow(a->hwnd);CHECK(!frames);
    puts("PASS: reorder final indices, cross-frame insertion, document identity/dirty/path/selection, busy/API/forged-drop refusal, last-source lifetime");
    return 0;
}
