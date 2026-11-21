#pragma once
#include <windows.h>
#include "api.h"
#include "textfile.h"

typedef struct {
    HWND edit;
    WCHAR path[MAX_PATH];
    TextFormat format;
    unsigned long long revision;
    BOOL dirty;
} Document;
void doc_init(Document *d, HWND edit);
void doc_changed(Document *d);
void doc_new(Document *d);
BOOL doc_open(Document *d,const WCHAR *path,WCHAR *reason,DWORD cap);
BOOL doc_save(Document *d,const WCHAR *path,BOOL createOnly,WCHAR *reason,DWORD cap);
void doc_dispatch(Document *d,ApiRequest *r,BOOL available);
WCHAR *doc_snapshot(HWND edit);
