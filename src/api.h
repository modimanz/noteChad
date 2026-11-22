#ifndef NOTECHAD_API_H
#define NOTECHAD_API_H
#include <windows.h>

#define WM_NOTECHAD_API (WM_APP + 20)
#define WM_NOTECHAD_EOF (WM_APP + 21)
#define NOTECHAD_API_MAX_BYTES (32UL * 1024UL * 1024UL)
#define NOTECHAD_API_MAX_HEADER 128

typedef struct {
    char verb[8];
    unsigned long long rev, start, end;
    WCHAR *payload;
    BOOL ok, dirty;
    unsigned long long current;
    WCHAR *reply;
} ApiRequest;

/* Call on owner GUI thread after creating the editor. One successful start/process.
 * stdin/stdout must be pipes and become transport-owned until worker exit.
 * In WM_NOTECHAD_API, call api_owns(lParam) ONCE before any dereference.
 * It atomically consumes the published worker request, only on the owner thread.
 * Fill ok/current/dirty/reply synchronously. reply must use LocalAlloc or be NULL.
 * payload is immutable and transport-owned; neither pointer may be retained.
 * WM_NOTECHAD_EOF has no payload; the owner decides hidden/visible lifecycle.
 */
BOOL api_start(HWND owner);
BOOL api_owns(LPARAM request);
#endif
