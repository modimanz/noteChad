/* Optional, explicitly requested local completion; no startup networking. */
#include "ai.h"
#include <winhttp.h>
#include <strsafe.h>
#include <string.h>
#define AI_CONTEXT 2048
#define AI_OUTPUT 4096
#define AI_BODY 65536
#define AI_PROPERTY L"noteChad.ai.request"
typedef struct AiWork {
    HWND owner, edit;
    unsigned long long revision, generation;
    DWORD caret;
    int length;
    INTERNET_PORT port;
    WCHAR model[129], context[AI_CONTEXT + 1];
    WCHAR suggestion[AI_OUTPUT + 1], error[512];
} AiWork;
static BOOL ai_on;
static unsigned long long ai_generation;
static volatile LONG ai_busy;
/* A window-message pointer is atomically compared/consumed before dereference. */
static PVOID volatile ai_ready;
static void ai_notice(HWND owner, const WCHAR *text)
{
    MessageBoxW(owner, text, L"noteChad", MB_OK | MB_ICONINFORMATION);
}
BOOL ai_enabled(void) { return ai_on; }
void ai_toggle(HWND owner)
{
    ai_on = !ai_on;
    ++ai_generation;
    ai_notice(owner, ai_on ? L"AI completion is on for this session. Request completion sends at most 2048 preceding UTF-16 units to local Ollama. Review and explicitly approve before anything is inserted." : L"AI completion is off. Pending suggestions will not be inserted.");
}
static void ai_free(AiWork *w)
{
    if (IsWindow(w->edit) && GetPropW(w->edit, AI_PROPERTY) == w) RemovePropW(w->edit, AI_PROPERTY);
    SecureZeroMemory(w, sizeof(*w));
    HeapFree(GetProcessHeap(), 0, w);
    InterlockedExchange(&ai_busy, 0);
}
static BOOL ai_valid_text(const WCHAR *s)
{
    while (*s) {
        WCHAR c = *s++;
        if (c >= 0xd800 && c <= 0xdbff) {
            if (*s < 0xdc00 || *s > 0xdfff) return FALSE;
            ++s;
        } else if (c >= 0xdc00 && c <= 0xdfff) return FALSE;
        else if (c < 32 && c != L'\r' && c != L'\n' && c != L'\t') return FALSE;
    }
    return TRUE;
}
/* Borrow the classic Unicode multiline EDIT buffer, copying only bounded context;
   never move selection or allocate a copy of the entire document. */
static BOOL ai_snapshot(HWND edit, DWORD caret, WCHAR *out)
{
    HLOCAL handle = (HLOCAL)SendMessageW(edit, EM_GETHANDLE, 0, 0);
    SIZE_T size;
    const WCHAR *text;
    DWORD first = caret > AI_CONTEXT ? caret - AI_CONTEXT : 0;
    DWORD count;
    int length = GetWindowTextLengthW(edit);
    if (!handle || length < 0 || caret > (DWORD)length) return FALSE;
    size = LocalSize(handle);
    if (size / sizeof(WCHAR) <= (SIZE_T)length) return FALSE;
    text = (const WCHAR *)LocalLock(handle);
    if (!text) return FALSE;
    if (first < caret && text[first] >= 0xdc00 && text[first] <= 0xdfff) ++first;
    count = caret - first;
    CopyMemory(out, text + first, count * sizeof(WCHAR)); out[count] = 0;
    LocalUnlock(handle);
    return ai_valid_text(out);
}
static BOOL ai_still_current(AiWork *w, HWND owner, HWND edit, unsigned long long revision)
{
    DWORD a = 0, b = 0;
    WCHAR context[AI_CONTEXT + 1];
    if (!ai_on || w->generation != ai_generation || w->owner != owner || w->edit != edit ||
        w->revision != revision || !IsWindow(edit) || GetPropW(edit, AI_PROPERTY) != w ||
        !IsWindowEnabled(owner) || !IsWindowEnabled(edit) || (GetWindowLongPtrW(edit, GWL_STYLE) & ES_READONLY)) return FALSE;
    SendMessageW(edit, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
    return a == b && a == w->caret && GetWindowTextLengthW(edit) == w->length &&
        ai_snapshot(edit, a, context) && !lstrcmpW(context, w->context);
}
/* Strict bounded JSON reader: skip/validate unknown fields instead of substring
   matching. Require a top-level response string and done:true. */
typedef struct AiJson { const WCHAR *p; int depth; } AiJson;
static void ai_ws(AiJson *j)
{
    while (*j->p == L' ' || *j->p == L'\t' || *j->p == L'\r' || *j->p == L'\n') ++j->p;
}
static int ai_hex(WCHAR c)
{
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}
static BOOL ai_u4(AiJson *j, WCHAR *out)
{
    unsigned n = 0;
    int i, h;
    for (i = 0; i < 4; ++i) {
        h = ai_hex(*j->p); if (h < 0) return FALSE;
        n = n * 16 + (unsigned)h; ++j->p;
    }
    *out = (WCHAR)n; return TRUE;
}
static BOOL ai_string(AiJson *j, WCHAR *out, unsigned cap)
{
    unsigned n = 0;
    WCHAR high = 0;
    if (*j->p++ != L'"') return FALSE;
    while (*j->p && *j->p != L'"') {
        WCHAR c = *j->p++;
        if (c < 32) return FALSE;
        if (c == L'\\') {
            c = *j->p++;
            switch (c) {
            case L'"': case L'\\': case L'/': break;
            case L'b': c = L'\b'; break;
            case L'f': c = L'\f'; break;
            case L'n': c = L'\n'; break;
            case L'r': c = L'\r'; break;
            case L't': c = L'\t'; break;
            case L'u': if (!ai_u4(j, &c)) return FALSE; break;
            default: return FALSE;
            }
        }
        if (!c) return FALSE;
        if (high) { if (c < 0xdc00 || c > 0xdfff) return FALSE; high = 0; }
        else if (c >= 0xd800 && c <= 0xdbff) high = c;
        else if (c >= 0xdc00 && c <= 0xdfff) return FALSE;
        if (out) { if (n + 1 >= cap) return FALSE; out[n] = c; }
        ++n;
    }
    if (*j->p != L'"' || high) return FALSE;
    ++j->p; if (out) out[n] = 0; return TRUE;
}
static BOOL ai_literal(AiJson *j, const WCHAR *s)
{
    const WCHAR *p = j->p;
    while (*s) { if (*p++ != *s++) return FALSE; }
    j->p = p; return TRUE;
}
static BOOL ai_skip(AiJson *j)
{
    WCHAR close;
    ai_ws(j); if (++j->depth > 16) return FALSE;
    if (*j->p == L'"') { if (!ai_string(j, NULL, 0)) return FALSE; }
    else if (*j->p == L'{' || *j->p == L'[') {
        close = *j->p++ == L'{' ? L'}' : L']'; ai_ws(j);
        if (*j->p != close) for (;;) {
            if (close == L'}') {
                if (!ai_string(j, NULL, 0)) return FALSE;
                ai_ws(j); if (*j->p++ != L':') return FALSE;
            }
            if (!ai_skip(j)) return FALSE;
            ai_ws(j); if (*j->p != L',') break;
            ++j->p; ai_ws(j);
        }
        if (*j->p++ != close) return FALSE;
    } else if (*j->p == L't') { if (!ai_literal(j, L"true")) return FALSE; }
    else if (*j->p == L'f') { if (!ai_literal(j, L"false")) return FALSE; }
    else if (*j->p == L'n') { if (!ai_literal(j, L"null")) return FALSE; }
    else {
        if (*j->p == L'-') ++j->p;
        if (*j->p == L'0') ++j->p;
        else { if (*j->p < L'1' || *j->p > L'9') return FALSE; while (*j->p >= L'0' && *j->p <= L'9') ++j->p; }
        if (*j->p == L'.') { ++j->p; if (*j->p < L'0' || *j->p > L'9') return FALSE; while (*j->p >= L'0' && *j->p <= L'9') ++j->p; }
        if (*j->p == L'e' || *j->p == L'E') {
            ++j->p; if (*j->p == L'+' || *j->p == L'-') ++j->p;
            if (*j->p < L'0' || *j->p > L'9') return FALSE;
            while (*j->p >= L'0' && *j->p <= L'9') ++j->p;
        }
    }
    --j->depth; return TRUE;
}
static BOOL ai_parse(const char *body, int count, WCHAR *out)
{
    WCHAR *wide, key[256], raw[AI_OUTPUT + 1];
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, body, count, NULL, 0);
    AiJson j;
    BOOL response = FALSE, done = FALSE, ok = FALSE;
    unsigned n = 0, i;
    if (!len) return FALSE;
    wide = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ((SIZE_T)len + 1) * sizeof(WCHAR));
    if (!wide) return FALSE;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, body, count, wide, len)) goto end;
    for (i = 0; i < (unsigned)len; ++i) if (!wide[i]) goto end;
    j.p = wide; j.depth = 0; ai_ws(&j); if (*j.p++ != L'{') goto end; ai_ws(&j);
    if (*j.p != L'}') for (;;) {
        if (!ai_string(&j, key, 256)) goto end;
        ai_ws(&j); if (*j.p++ != L':') goto end; ai_ws(&j);
        if (!lstrcmpW(key, L"response")) {
            if (response || !ai_string(&j, raw, AI_OUTPUT + 1)) goto end;
            response = TRUE;
        } else if (!lstrcmpW(key, L"done")) {
            if (done || !ai_literal(&j, L"true")) goto end;
            done = TRUE;
        } else if (!lstrcmpW(key, L"error")) goto end;
        else if (!ai_skip(&j)) goto end;
        ai_ws(&j); if (*j.p != L',') break;
        ++j.p; ai_ws(&j);
    }
    if (*j.p++ != L'}') goto end;
    ai_ws(&j);
    if (*j.p || !response || !done || !raw[0] || !ai_valid_text(raw)) goto end;
    for (i = 0; raw[i]; ++i) {
        if (raw[i] == L'\n' && (i == 0 || raw[i - 1] != L'\r')) {
            if (n >= AI_OUTPUT) goto end; out[n++] = L'\r';
        }
        if (n >= AI_OUTPUT) goto end; out[n++] = raw[i];
    }
    out[n] = 0; ok = TRUE;
end:
    SecureZeroMemory(wide, ((SIZE_T)len + 1) * sizeof(WCHAR));
    HeapFree(GetProcessHeap(), 0, wide); return ok;
}
/* ASCII escapes preserve all valid UTF-16, including surrogate pairs. */
static char *ai_quote(char *p, const WCHAR *s)
{
    static const char hex[] = "0123456789abcdef";
    *p++ = '"';
    while (*s) {
        unsigned c = *s++;
        if (c >= 32 && c < 127 && c != '"' && c != '\\') *p++ = (char)c;
        else {
            *p++ = '\\'; *p++ = 'u';
            *p++ = hex[(c >> 12) & 15]; *p++ = hex[(c >> 8) & 15];
            *p++ = hex[(c >> 4) & 15]; *p++ = hex[c & 15];
        }
    }
    *p++ = '"'; return p;
}
static DWORD WINAPI ai_worker(void *arg)
{
    AiWork *w = (AiWork *)arg;
    HWND destination = w->owner;
    HINTERNET session = NULL, connection = NULL, request = NULL;
    char *payload = NULL, *body = NULL, *p;
    DWORD flags = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    DWORD policy = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH, status, size = sizeof(status), got, used = 0;
    DWORD error = ERROR_SUCCESS;
    ULONGLONG start = GetTickCount64();
    payload = (char *)HeapAlloc(GetProcessHeap(), 0, 16384);
    body = (char *)HeapAlloc(GetProcessHeap(), 0, AI_BODY + 1);
    if (!payload || !body) { error = ERROR_NOT_ENOUGH_MEMORY; goto finish; }
    p = payload; CopyMemory(p, "{\"model\":", 9); p += 9; p = ai_quote(p, w->model);
    CopyMemory(p, ",\"prompt\":", 10); p += 10; p = ai_quote(p, w->context);
    {
        const char tail[] = ",\"system\":\"Continue the text at the caret. Return only the continuation, without repeating the prompt or adding explanations.\",\"stream\":false,\"options\":{\"num_predict\":128}}";
        CopyMemory(p, tail, sizeof(tail)); p += sizeof(tail) - 1;
    }
    session = WinHttpOpen(L"noteChad/1", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) goto failed;
    if (!WinHttpSetTimeouts(session, 2000, 2000, 5000, 15000)) goto failed;
    connection = WinHttpConnect(session, L"127.0.0.1", w->port, 0); if (!connection) goto failed;
    request = WinHttpOpenRequest(connection, L"POST", L"/api/generate", NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!request || !WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &flags, sizeof(flags)) ||
        !WinHttpSetOption(request, WINHTTP_OPTION_AUTOLOGON_POLICY, &policy, sizeof(policy))) goto failed;
    if (!WinHttpSendRequest(request, L"Content-Type: application/json\r\n", (DWORD)-1, payload, (DWORD)(p - payload), (DWORD)(p - payload), 0) ||
        !WinHttpReceiveResponse(request, NULL) ||
        !WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) goto failed;
    if (status != 200) {
        StringCchPrintfW(w->error, 512, L"Local Ollama returned HTTP %lu. Redirects are refused. For a missing model, choose an already installed model with NOTECHAD_MODEL (check `ollama list`) and restart noteChad. Nothing was inserted.", status); goto finish;
    }
    for (;;) {
        DWORD want = AI_BODY + 1 - used; if (want > 4096) want = 4096;
        if (GetTickCount64() - start > 30000) { error = ERROR_TIMEOUT; goto finish; }
        if (!WinHttpReadData(request, body + used, want, &got)) goto failed;
        used += got;
        if (used > AI_BODY) { StringCchCopyW(w->error, 512, L"Local Ollama response exceeded 64 KiB. Nothing was inserted."); goto finish; }
        if (!got) break;
    }
    if (!ai_parse(body, (int)used, w->suggestion)) StringCchCopyW(w->error, 512, L"Local Ollama returned an invalid, incomplete, empty, or oversized completion. Expected non-streaming JSON with response text and done:true. Nothing was inserted.");
    goto finish;
failed:
    error = GetLastError(); if (!error) error = ERROR_INVALID_DATA;
finish:
    if (error) StringCchPrintfW(w->error, 512, L"Could not complete the local Ollama request (Windows error %lu). Check that Ollama is running locally and NOTECHAD_MODEL names an installed model (`ollama list`). noteChad never downloads or launches models. Nothing was inserted.", error);
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    if (payload) { SecureZeroMemory(payload, 16384); HeapFree(GetProcessHeap(), 0, payload); }
    if (body) { SecureZeroMemory(body, AI_BODY + 1); HeapFree(GetProcessHeap(), 0, body); }
    InterlockedExchangePointer(&ai_ready, w);
    if (!PostMessageW(destination, WM_NOTECHAD_AI, 0, (LPARAM)w)) {
        if (InterlockedCompareExchangePointer(&ai_ready, NULL, w) == w) {
            /* No UI calls from a worker during teardown. */
            SecureZeroMemory(w, sizeof(*w)); HeapFree(GetProcessHeap(), 0, w);
            InterlockedExchange(&ai_busy, 0);
        }
    }
    return 0;
}
void ai_request(HWND owner, HWND edit, unsigned long long revision)
{
    AiWork *w;
    DWORD a = 0, b = 0, count;
    WCHAR port[16]; unsigned value = 0, i;
    HANDLE thread;
    if (!ai_on) return;
    if (!IsWindow(owner) || !IsWindow(edit) || !IsWindowUnicode(edit) ||
        !IsWindowEnabled(owner) || !IsWindowEnabled(edit) ||
        !(GetWindowLongPtrW(edit, GWL_STYLE) & ES_MULTILINE) ||
        (GetWindowLongPtrW(edit, GWL_STYLE) & ES_READONLY)) {
        ai_notice(owner, L"AI completion needs an editable Unicode multiline document."); return;
    }
    if (InterlockedCompareExchange(&ai_busy, 1, 0)) { ai_notice(owner, L"A local completion is already pending. Please wait for its result."); return; }
    w = (AiWork *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*w));
    if (!w) { InterlockedExchange(&ai_busy, 0); ai_notice(owner, L"Not enough memory for local completion."); return; }
    w->owner = owner; w->edit = edit; w->revision = revision; w->generation = ai_generation;
    SendMessageW(edit, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
    if (a != b) { ai_notice(owner, L"Clear the selection and place the caret where you want to append a completion. Selected text is never replaced."); ai_free(w); return; }
    w->caret = a; w->length = GetWindowTextLengthW(edit);
    if (!ai_snapshot(edit, a, w->context)) { ai_notice(owner, L"Could not safely read the text before the caret. Move the caret to a complete character and try again."); ai_free(w); return; }
    count = GetEnvironmentVariableW(L"NOTECHAD_MODEL", w->model, 129);
    if (!count || count >= 129 || !ai_valid_text(w->model)) {
        ai_notice(owner, L"Choose an already installed local Ollama model: run `ollama list`, set NOTECHAD_MODEL to its exact name, then restart noteChad. The model name must be at most 128 UTF-16 units. noteChad never installs, downloads, or launches models."); ai_free(w); return;
    }
    SetLastError(ERROR_SUCCESS); count = GetEnvironmentVariableW(L"NOTECHAD_OLLAMA_PORT", port, 16);
    if (!count && GetLastError() == ERROR_ENVVAR_NOT_FOUND) w->port = 11434;
    else {
        if (!count || count >= 16) goto bad_port;
        for (i = 0; i < count; ++i) {
            if (port[i] < L'0' || port[i] > L'9') goto bad_port;
            value = value * 10 + (unsigned)(port[i] - L'0'); if (value > 65535) goto bad_port;
        }
        if (!value) goto bad_port; w->port = (INTERNET_PORT)value;
    }
    if (!SetPropW(edit, AI_PROPERTY, w)) { ai_notice(owner, L"Could not track the document for safe completion."); ai_free(w); return; }
    thread = CreateThread(NULL, 0, ai_worker, w, 0, NULL);
    if (!thread) { ai_notice(owner, L"Could not start the local completion worker."); ai_free(w); return; }
    CloseHandle(thread); return;
bad_port:
    ai_notice(owner, L"NOTECHAD_OLLAMA_PORT must contain only digits and be between 1 and 65535. The host is always 127.0.0.1."); ai_free(w);
}
void ai_result(HWND owner, HWND edit, unsigned long long revision, LPARAM result)
{
    AiWork *w;
    WCHAR preview[AI_OUTPUT + 256];
    if (!result || InterlockedCompareExchangePointer(&ai_ready, NULL, (PVOID)result) != (PVOID)result) return;
    w = (AiWork *)result;
    if (w->owner != owner) { ai_free(w); return; }
    if (!ai_still_current(w, owner, edit, revision)) {
        ai_notice(owner, L"Completion discarded: AI was disabled, or the document, revision, caret, or selection changed."); ai_free(w); return;
    }
    if (w->error[0]) { ai_notice(owner, w->error); ai_free(w); return; }
    StringCchPrintfW(preview, AI_OUTPUT + 256, L"Insert this suggestion at the unchanged caret?\r\n\r\n%s\r\n\r\nChoose Yes to insert. No (the default) leaves your document unchanged.", w->suggestion);
    if (MessageBoxW(owner, preview, L"noteChad - Review local AI suggestion", MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) == IDYES) {
        /* Owner is modal-disabled here: refuse API document edits while disabled,
           and supply the current revision to ai_result. */
        if (ai_still_current(w, owner, edit, revision)) {
            SendMessageW(edit, EM_REPLACESEL, TRUE, (LPARAM)w->suggestion);
            SendMessageW(edit, EM_SCROLLCARET, 0, 0);
        } else ai_notice(owner, L"Completion discarded because its insertion point changed during review.");
    }
    ai_free(w);
}
