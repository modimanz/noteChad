/* Share GUI ownership only between ordinary launches of this executable. The
   only cross-process payload is a bounded list of file names, never documents,
   window handles, editor commands or pointers. API sessions never register. */
#include "router.h"
#include <strsafe.h>
#include <wchar.h>
#include <string.h>
#define ROUTE_CLASS L"NodeChad.Launch.v1"
#define ROUTE_COPY_ID 0x4e436831u
#define ROUTE_DRAIN (WM_APP+51)
#define MAX_FILES 64
#define MAX_PENDING 16

typedef struct Pending { struct Pending *next; int count; WCHAR data[1]; } Pending;
static HWND inbox;
static HANDLE lock;
static RouterOpen open_callback;
static Pending *first,*last;
static unsigned queued;
static BOOL draining;

/* NUL-separated absolute paths with one extra terminal NUL. Empty request is
   exactly one NUL: open a new, blank editor window. */
static Pending *unpack(const COPYDATASTRUCT *c){
    if(!c || c->dwData!=ROUTE_COPY_ID || !c->lpData || !c->cbData || (c->cbData%sizeof(WCHAR)) || c->cbData>(MAX_PATH*MAX_FILES+1)*sizeof(WCHAR))return NULL;
    const WCHAR *text=c->lpData; size_t end=c->cbData/sizeof(WCHAR),i=0; int count=0;
    while(i<end && text[i]){
        size_t start=i; while(i<end && text[i])++i;
        if(i==end || i-start>=MAX_PATH || ++count>MAX_FILES)return NULL;
        ++i;
    }
    if(i!=end-1 || text[i]!=0)return NULL;
    Pending *p=LocalAlloc(LMEM_FIXED,sizeof(*p)+c->cbData);if(!p)return NULL;
    p->next=NULL;p->count=count;memcpy(p->data,text,c->cbData);return p;
}
static LRESULT CALLBACK receive(HWND h,UINT m,WPARAM w,LPARAM l){
    if(m==WM_COPYDATA){
        if(queued>=MAX_PENDING || !open_callback)return FALSE;
        Pending *p=unpack((const COPYDATASTRUCT *)l);if(!p)return FALSE;
        if(!PostMessageW(h,ROUTE_DRAIN,0,0)){LocalFree(p);return FALSE;}
        if(last)last->next=p;else first=p;last=p;++queued;return TRUE;
    }
    if(m==ROUTE_DRAIN){
        if(draining)return 0;draining=TRUE;
        while(first && open_callback){
            Pending *p=first;first=p->next;if(!first)last=NULL;--queued;
            const WCHAR *names[MAX_FILES];WCHAR *s=p->data;
            for(int n=0;n<p->count;++n){names[n]=s;s+=wcslen(s)+1;}
            open_callback(p->count,names);LocalFree(p);
        }
        draining=FALSE;return 0;
    }
    return DefWindowProcW(h,m,w,l);
}
static BOOL same_exe(HWND h,const WCHAR *self,DWORD *pid){
    GetWindowThreadProcessId(h,pid);HANDLE proc=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,*pid);
    if(!proc)return FALSE;
    WCHAR path[1024];DWORD n=ARRAYSIZE(path);BOOL ok=QueryFullProcessImageNameW(proc,0,path,&n) && CompareStringOrdinal(path,-1,self,-1,TRUE)==CSTR_EQUAL;
    CloseHandle(proc);return ok;
}
static BOOL forward(HWND h,DWORD pid,int argc,WCHAR **argv){
    if(argc<1 || argc>MAX_FILES+1)return FALSE;
    WCHAR *data=LocalAlloc(LPTR,((SIZE_T)(argc-1)*MAX_PATH+1)*sizeof(WCHAR));if(!data)return FALSE;
    WCHAR *next=data;BOOL ok=FALSE;
    for(int i=1;i<argc;++i){DWORD n=GetFullPathNameW(argv[i],MAX_PATH,next,NULL);if(!n||n>=MAX_PATH)goto done;next+=n+1;}
    COPYDATASTRUCT c={ROUTE_COPY_ID,(DWORD)((next-data+1)*sizeof(WCHAR)),data};DWORD_PTR ack=0;
    AllowSetForegroundWindow(pid);
    ok=SendMessageTimeoutW(h,WM_COPYDATA,0,(LPARAM)&c,SMTO_ABORTIFHUNG|SMTO_BLOCK|SMTO_ERRORONEXIT,4000,&ack)!=0 && ack==TRUE;
done:LocalFree(data);return ok;
}
int router_start(int argc,WCHAR **argv,RouterOpen cb){
    WCHAR self[1024],name[80];DWORD len=GetModuleFileNameW(NULL,self,ARRAYSIZE(self));if(!len || len>=ARRAYSIZE(self))return ROUTER_ERROR;
    DWORD hash=2166136261u;for(DWORD i=0;i<len;++i){WCHAR c=self[i];if(c>=L'A'&&c<=L'Z')c+=32;hash=(hash^c)*16777619u;}
    StringCchPrintfW(name,ARRAYSIZE(name),L"Local\\nodeChad.GUI.v1.%08lX",hash);
    lock=CreateMutexW(NULL,FALSE,name);if(!lock)return ROUTER_ERROR;
    DWORD got=WaitForSingleObject(lock,5000);if(got!=WAIT_OBJECT_0 && got!=WAIT_ABANDONED){CloseHandle(lock);lock=NULL;return ROUTER_ERROR;}
    int result=ROUTER_ERROR;
    HWND other=NULL;DWORD pid=0;while((other=FindWindowExW(HWND_MESSAGE,other,ROUTE_CLASS,name+6))!=NULL){if(same_exe(other,self,&pid))break;}
    if(other){/* Never hold the startup lock while calling into the first process. */
        ReleaseMutex(lock);CloseHandle(lock);lock=NULL;
        return forward(other,pid,argc,argv)?ROUTER_FORWARDED:ROUTER_ERROR;
    }
    WNDCLASSW wc={0};wc.hInstance=GetModuleHandleW(NULL);wc.lpfnWndProc=receive;wc.lpszClassName=ROUTE_CLASS;
    if(RegisterClassW(&wc)||GetLastError()==ERROR_CLASS_ALREADY_EXISTS){
        open_callback=cb;inbox=CreateWindowExW(0,ROUTE_CLASS,name+6,0,0,0,0,0,HWND_MESSAGE,NULL,wc.hInstance,NULL);
        if(inbox)result=ROUTER_PRIMARY;
    }
    ReleaseMutex(lock);if(result!=ROUTER_PRIMARY){CloseHandle(lock);lock=NULL;open_callback=NULL;}return result;
}
void router_stop(void){
    open_callback=NULL;if(inbox){DestroyWindow(inbox);inbox=NULL;}
    while(first){Pending *p=first;first=p->next;LocalFree(p);}last=NULL;queued=0;
    if(lock){CloseHandle(lock);lock=NULL;}
}
