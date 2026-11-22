/* Transport-only coordinator: real GUI thread, deterministic document stub. */
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <wchar.h>
#include <string.h>
#include "../src/api.h"
static WCHAR document[4096];
static unsigned long long revision = 1;
static BOOL dirty;
static DWORD gui_thread;
static WCHAR *copy(const WCHAR *text) {
    SIZE_T n = (wcslen(text) + 1) * sizeof(WCHAR);
    WCHAR *p = (WCHAR *)LocalAlloc(LMEM_FIXED, n);
    if (p) CopyMemory(p, text, n);
    return p;
}
static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_NOTECHAD_API) {
        ApiRequest *r;
        if (!api_owns(lp)) return 0;
        r = (ApiRequest *)lp;
        if (api_owns(lp) || GetCurrentThreadId() != gui_thread) ExitProcess(90);
        r->ok = TRUE;
        if (strcmp(r->verb, "read") == 0) r->reply = copy(document);
        else if (r->rev != revision) { r->ok = FALSE; r->reply = copy(L"revision conflict"); }
        else if (strcmp(r->verb, "write") == 0) {
            if (wcslen(r->payload) >= 4096) { r->ok = FALSE; r->reply = copy(L"stub capacity"); }
            else { wcscpy_s(document, 4096, r->payload); revision++; dirty = TRUE;
                   r->reply = copy(L"successful mutation body must be suppressed"); }
        } else { r->ok = FALSE; r->reply = copy(L"stub only implements read/write"); }
        r->current = revision;
        r->dirty = dirty;
        return 0;
    }
    if (message == WM_NOTECHAD_EOF) { DestroyWindow(window); return 0; }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wp, lp);
}
int main(void) {
    WNDCLASSW wc = {0};
    HWND owner;
    MSG message;
    gui_thread = GetCurrentThreadId();
    wc.lpfnWndProc = procedure; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"NoteChadApiTest";
    if (!RegisterClassW(&wc)) return 2;
    owner = CreateWindowW(wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    if (!owner || api_owns((LPARAM)1) || !api_start(owner)) return 3;
    if (api_start(owner)) return 4;
    while (GetMessageW(&message, NULL, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    return 0;
}
