#include "document.h"
#include <wchar.h>
#include <string.h>

int doc_offset(const WCHAR *, unsigned long long);
static void note(WCHAR *out,DWORD cap,const WCHAR *msg) { if(cap) lstrcpynW(out,msg,(int)cap); }
static WCHAR *copy(const WCHAR *s) {
    size_t n=(wcslen(s)+1)*sizeof(WCHAR); WCHAR *p=LocalAlloc(LMEM_FIXED,n);
    if(p) memcpy(p,s,n); return p;
}
WCHAR *doc_snapshot(HWND edit) {
    int n=GetWindowTextLengthW(edit); WCHAR *p=LocalAlloc(LMEM_FIXED,((size_t)n+1)*sizeof(WCHAR));
    if (p && GetWindowTextW(edit,p,n+1)!=n) {LocalFree(p); return NULL;} return p;
}
void doc_changed(Document *d) { ++d->revision; d->dirty=TRUE; }
void doc_init(Document *d, HWND edit) { d->edit=edit; d->revision=1; tf_default(&d->format); }
void doc_new(Document *d) {
    if(!SetWindowTextW(d->edit,L"")) return;
    d->path[0]=0; tf_default(&d->format); d->dirty=FALSE; ++d->revision;
    SendMessageW(d->edit,EM_EMPTYUNDOBUFFER,0,0);
}
static BOOL path_ok(const WCHAR *path,WCHAR *full,WCHAR *reason,DWORD cap) {
    if(!path || !*path) {note(reason,cap,L"A file path is required.");return FALSE;}
    DWORD n=GetFullPathNameW(path,MAX_PATH,full,NULL);
    if(!n||n>=MAX_PATH) {note(reason,cap,L"Use a path shorter than 260 characters.");return FALSE;}
    return TRUE;
}
BOOL doc_open(Document *d,const WCHAR *path,WCHAR *reason,DWORD cap) {
    WCHAR full[MAX_PATH],*text=NULL; TextFormat f;
    if(!path_ok(path,full,reason,cap)||!tf_load(full,&text,&f,reason,cap)) return FALSE;
    BOOL ok=SetWindowTextW(d->edit,text); LocalFree(text);
    if(!ok){note(reason,cap,L"The edit control could not load the file.");return FALSE;}
    d->format=f; lstrcpyW(d->path,full); d->dirty=FALSE; ++d->revision;
    SendMessageW(d->edit,EM_EMPTYUNDOBUFFER,0,0); return TRUE;
}
BOOL doc_save(Document *d,const WCHAR *path,BOOL createOnly,WCHAR *reason,DWORD cap) {
    WCHAR full[MAX_PATH]; if(!path_ok(path,full,reason,cap)) return FALSE;
    WCHAR *text=doc_snapshot(d->edit); if(!text){note(reason,cap,L"Cannot read the entire document.");return FALSE;}
    BOOL current=d->path[0] && !_wcsicmp(d->path,full);
    BOOL ok=(!current && createOnly)?tf_save_new(full,text,&d->format,reason,cap):tf_save(full,text,&d->format,current,reason,cap);
    LocalFree(text); if(!ok) return FALSE;
    lstrcpyW(d->path,full); d->dirty=FALSE; ++d->revision; return TRUE;
}
/* API paths target ordinary local-drive files; do not open devices, ADS or UNC pipes. */
static BOOL api_path(const WCHAR *s) {
    if(!(((s[0]>=L'A' && s[0]<=L'Z')||(s[0]>=L'a' && s[0]<=L'z')) && s[1]==L':' && (s[2]==L'\\'||s[2]==L'/'))) return FALSE;
    const WCHAR *p=s+3; if(!*p || wcspbrk(p,L":*?<>|\""))return FALSE;
    for(;;){
        size_t n=wcscspn(p,L"\\/"), stem=wcscspn(p,L".\\/");
        if(!n || p[n-1]=='.' || p[n-1]==' ')return FALSE;
        while(stem && p[stem-1]==' ')--stem;
        if ((stem==3 && (!_wcsnicmp(p,L"CON",3)||!_wcsnicmp(p,L"PRN",3)||!_wcsnicmp(p,L"AUX",3)||!_wcsnicmp(p,L"NUL",3))) ||
            (stem==4 && (!_wcsnicmp(p,L"COM",3)||!_wcsnicmp(p,L"LPT",3))) ||
            (stem==6 && !_wcsnicmp(p,L"CONIN$",6)) || (stem==7 && !_wcsnicmp(p,L"CONOUT$",7)))return FALSE;
        for(size_t i=0;i<n;++i)if(p[i]<32)return FALSE;
        if(!p[n])return TRUE; p+=n+1;
    }
}
/* The API counts Unicode scalar values after CRLF -> LF. EDIT uses UTF-16 offsets. */
static WCHAR *to_editor(const WCHAR *s) {
    size_t len=wcslen(s),n=0; if(len>TF_EDIT_LIMIT)return NULL;
    for(size_t i=0;i<len;++i) { ++n; if(s[i]=='\r'){if(s[i+1]!='\n')return NULL;} else if(s[i]=='\n' && (!i || s[i-1]!='\r')) ++n; }
    if(n>TF_EDIT_LIMIT)return NULL;
    WCHAR *out=LocalAlloc(LMEM_FIXED,(n+1)*sizeof(WCHAR)); if(!out)return NULL;
    WCHAR *w=out; for(size_t i=0;i<len;++i){ if(s[i]=='\n' && (!i||s[i-1]!='\r'))*w++='\r'; *w++=s[i]; } *w=0; return out;
}
static void lf_in_place(WCHAR *s) { WCHAR *w=s; for(;*s;++s){if(*s=='\r' && s[1]=='\n')++s; *w++=*s;} *w=0; }
void doc_dispatch(Document *d,ApiRequest *r,BOOL available) {
    WCHAR reason[512]=L"Unknown command."; WCHAR *old=NULL,*ins=NULL;
    if(!available) note(reason,512,L"Editor is in a dialog. Finish that action, then retry.");
    else if(!strcmp(r->verb,"read")) {
        r->reply=doc_snapshot(d->edit); if(r->reply){lf_in_place(r->reply);r->ok=TRUE;} else note(reason,512,L"Cannot read document.");
    } else if(!r->rev || r->rev!=d->revision) note(reason,512,L"Revision conflict. Read the document again.");
    else if(!strcmp(r->verb,"load")) {
        if(d->dirty) note(reason,512,L"Unsaved edits: save before loading another file.");
        else if(!api_path(r->payload)) note(reason,512,L"Use an absolute local drive file path.");
        else r->ok=doc_open(d,r->payload,reason,512);
    } else if(!strcmp(r->verb,"save")) {
        const WCHAR *path=*r->payload?r->payload:d->path;
        if(!api_path(path))note(reason,512,L"Use an absolute local drive file path.");
        else r->ok=doc_save(d,path,TRUE,reason,512);
    } else if(!strcmp(r->verb,"edit") || !strcmp(r->verb,"write")) {
        old=doc_snapshot(d->edit); ins=to_editor(r->payload);
        if(!ins||!old) note(reason,512,L"Text too large, unsupported CR-only newline, or insufficient memory.");
        else {
            int n=(int)wcslen(old),a=0,b=n,k=(int)wcslen(ins);
            if(!strcmp(r->verb,"edit")){a=doc_offset(old,r->start); b=doc_offset(old,r->end);}
            if(a<0||b<a||b>n)note(reason,512,L"Edit range outside document.");
            else if(n-(b-a)+k>TF_EDIT_LIMIT)note(reason,512,L"Document would exceed editor limit.");
            else {
                unsigned long long prev=d->revision;
                SendMessageW(d->edit,EM_SETSEL,a,b); SendMessageW(d->edit,EM_REPLACESEL,TRUE,(LPARAM)ins);
                if(GetWindowTextLengthW(d->edit)!=n-(b-a)+k)note(reason,512,L"Editor could not apply the full change; read again.");
                else {if(prev==d->revision)doc_changed(d); r->ok=TRUE;}
            }
        }
    }
    LocalFree(old); LocalFree(ins); if(!r->ok) r->reply=copy(reason);
    r->current=d->revision;r->dirty=d->dirty;
}
