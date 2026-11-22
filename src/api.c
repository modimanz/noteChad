#include "api.h"
#include <stdint.h>
#include <stdio.h>
#include <process.h>
#include <string.h>

static HWND api_owner;
static DWORD api_gui_thread;
static HANDLE api_input, api_output;
static LONG api_started;
static PVOID volatile api_pending;

BOOL api_owns(LPARAM request) {
    if (!request || GetCurrentThreadId() != api_gui_thread) return FALSE;
    return InterlockedCompareExchangePointer(&api_pending, NULL, (PVOID)request) == (PVOID)request;
}

/* ReadFile/WriteFile are deliberately blocking: no polling and no network. */
static BOOL read_exact(void *buffer, DWORD size) {
    BYTE *p = (BYTE *)buffer;
    while (size) {
        DWORD n = 0;
        if (!ReadFile(api_input, p, size, &n, NULL) || !n) return FALSE;
        p += n; size -= n;
    }
    return TRUE;
}
static BOOL write_exact(const void *buffer, DWORD size) {
    const BYTE *p = (const BYTE *)buffer;
    while (size) {
        DWORD n = 0;
        if (!WriteFile(api_output, p, size, &n, NULL) || !n) return FALSE;
        p += n; size -= n;
    }
    return TRUE;
}
static BOOL reply(BOOL ok, unsigned long long revision, BOOL dirty, const WCHAR *text) {
    char header[NOTECHAD_API_MAX_HEADER];
    char *bytes = NULL;
    SIZE_T length = 0;
    int count = 0, header_length;
    BOOL sent;
    if (text) {
        while (length <= NOTECHAD_API_MAX_BYTES && text[length]) ++length;
        if (length > NOTECHAD_API_MAX_BYTES) return FALSE;
        if (length) {
            count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, (int)length, NULL, 0, NULL, NULL);
            if (!count || count > (int)NOTECHAD_API_MAX_BYTES) return FALSE;
            bytes = (char *)LocalAlloc(LMEM_FIXED, (SIZE_T)count);
            if (!bytes) return FALSE;
            if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, (int)length, bytes, count, NULL, NULL)) {
                LocalFree(bytes); return FALSE;
            }
        }
    }
    header_length = sprintf_s(header, sizeof(header), "%s %llu %u %d\n", ok ? "OK" : "ERR", revision, dirty ? 1U : 0U, count);
    sent = header_length > 0 && write_exact(header, (DWORD)header_length) && write_exact(bytes, (DWORD)count);
    if (bytes) LocalFree(bytes);
    return sent;
}
static BOOL number(char **cursor, unsigned long long *value, char terminator) {
    char *p = *cursor;
    unsigned long long n = 0;
    unsigned int digits = 0;
    while (*p >= '0' && *p <= '9') {
        unsigned int digit = (unsigned int)(*p - '0');
        if (++digits > 20 || n > (UINT64_MAX - digit) / 10) return FALSE;
        n = n * 10 + digit; ++p;
    }
    if (!digits || *p != terminator) return FALSE;
    *value = n; *cursor = p + 1;
    return TRUE;
}
static BOOL parse(char *header, ApiRequest *r, DWORD *size) {
    char *p = header, *space = strchr(header, ' ');
    SIZE_T length;
    unsigned long long bytes;
    if (!space) return FALSE;
    length = (SIZE_T)(space - p);
    if (!length || length >= sizeof(r->verb)) return FALSE;
    CopyMemory(r->verb, p, length); r->verb[length] = 0; p = space + 1;
    if (strcmp(r->verb, "read") && strcmp(r->verb, "write") && strcmp(r->verb, "edit") &&
        strcmp(r->verb, "save") && strcmp(r->verb, "load")) return FALSE;
    if (!number(&p, &r->rev, ' ') || !number(&p, &r->start, ' ') ||
        !number(&p, &r->end, ' ') || !number(&p, &bytes, '\0') || bytes > NOTECHAD_API_MAX_BYTES) return FALSE;
    if (!strcmp(r->verb, "read") && (r->rev || r->start || r->end || bytes)) return FALSE;
    if (strcmp(r->verb, "edit") && (r->start || r->end)) return FALSE;
    if (!strcmp(r->verb, "edit") && r->start > r->end) return FALSE;
    if (!strcmp(r->verb, "load") && !bytes) return FALSE;
    *size = (DWORD)bytes;
    return TRUE;
}
/* Malformed packets never reach the coordinator. A separate, valid read takes
 * an authoritative status snapshot before a terminal transport error. */
static void transport_error(unsigned long long *current, BOOL *dirty, const WCHAR *reason) {
    ApiRequest status;
    WCHAR empty[1] = {0};
    ZeroMemory(&status, sizeof(status));
    CopyMemory(status.verb, "read", 5);
    status.payload = empty;
    status.current = *current; status.dirty = *dirty;
    InterlockedExchangePointer(&api_pending, &status);
    SendMessageW(api_owner, WM_NOTECHAD_API, 0, (LPARAM)&status);
    if (!InterlockedExchangePointer(&api_pending, NULL)) {
        *current = status.current; *dirty = status.dirty;
    }
    if (status.reply) LocalFree(status.reply);
    (void)reply(FALSE, *current, *dirty, reason);
}
static unsigned int __stdcall worker(void *ignored) {
    unsigned long long current = 0;
    BOOL dirty = FALSE;
    (void)ignored;
    for (;;) {
        char header[NOTECHAD_API_MAX_HEADER];
        DWORD used = 0, size = 0;
        char ch, *utf8 = NULL;
        int wide_count;
        ApiRequest r;
        const WCHAR *error = NULL;
        BOOL eof = FALSE;
        ZeroMemory(&r, sizeof(r));
        for (;;) {
            if (!read_exact(&ch, 1)) { eof = TRUE; break; }
            if (ch == '\n') { header[used] = 0; break; }
            if ((unsigned char)ch < 32 || (unsigned char)ch > 126 || used >= sizeof(header) - 1) {
                error = L"invalid or overlong header"; break;
            }
            header[used++] = ch;
        }
        if (eof) {
            if (used) transport_error(&current, &dirty, L"truncated header");
            break;
        }
        if (!error && !parse(header, &r, &size)) error = L"invalid request header";
        if (error) { transport_error(&current, &dirty, error); break; }
        utf8 = (char *)LocalAlloc(LMEM_FIXED, (SIZE_T)size + 1);
        if (!utf8) { transport_error(&current, &dirty, L"out of memory"); break; }
        if (!read_exact(utf8, size)) error = L"truncated payload";
        utf8[size] = 0;
        wide_count = 0;
        if (!error && size) {
            if (memchr(utf8, 0, size)) error = L"NUL is not allowed";
            else {
                wide_count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, (int)size, NULL, 0);
                if (!wide_count) error = L"invalid UTF-8";
            }
        }
        if (!error) {
            r.payload = (WCHAR *)LocalAlloc(LMEM_FIXED, ((SIZE_T)wide_count + 1) * sizeof(WCHAR));
            if (!r.payload) error = L"out of memory";
            else {
                if (wide_count && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, (int)size, r.payload, wide_count))
                    error = L"invalid UTF-8";
                r.payload[wide_count] = 0;
            }
        }
        LocalFree(utf8);
        if (error) {
            if (r.payload) LocalFree(r.payload);
            transport_error(&current, &dirty, error); break;
        }
        r.current = current; r.dirty = dirty;
        InterlockedExchangePointer(&api_pending, &r);
        SendMessageW(api_owner, WM_NOTECHAD_API, 0, (LPARAM)&r);
        if (InterlockedExchangePointer(&api_pending, NULL)) {
            r.ok = FALSE;
            error = L"editor did not claim request";
        }
        current = r.current; dirty = r.dirty;
        {
            const WCHAR *body = error ? error : r.reply;
            BOOL sent;
            if (r.ok && strcmp(r.verb, "read")) body = NULL;
            sent = reply(r.ok, current, dirty, body);
            if (!sent) (void)reply(FALSE, current, dirty, L"invalid or oversized editor reply");
            if (r.reply) LocalFree(r.reply);
            LocalFree(r.payload);
            if (!sent || error) break;
        }
    }
    CloseHandle(api_input); CloseHandle(api_output);
    SetStdHandle(STD_INPUT_HANDLE, NULL); SetStdHandle(STD_OUTPUT_HANDLE, NULL);
    PostMessageW(api_owner, WM_NOTECHAD_EOF, 0, 0);
    return 0;
}
BOOL api_start(HWND owner) {
    HANDLE thread;
    DWORD process = 0;
    DWORD gui = GetWindowThreadProcessId(owner, &process);
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE), output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!owner || gui != GetCurrentThreadId() || process != GetCurrentProcessId() ||
        GetFileType(input) != FILE_TYPE_PIPE || GetFileType(output) != FILE_TYPE_PIPE) return FALSE;
    if (InterlockedCompareExchange(&api_started, 1, 0)) return FALSE;
    api_owner = owner; api_gui_thread = gui; api_input = input; api_output = output;
    thread = (HANDLE)_beginthreadex(NULL, 0, worker, NULL, 0, NULL);
    if (!thread) { InterlockedExchange(&api_started, 0); return FALSE; }
    CloseHandle(thread);
    return TRUE;
}
