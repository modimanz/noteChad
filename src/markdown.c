#include "markdown.h"
#include <stdlib.h>
#include <wchar.h>
#include <string.h>
#include <limits.h>
#include <commctrl.h>
#pragma warning(push)
#pragma warning(disable:4201) /* Windows SDK anonymous unions. */
#include <richedit.h>
#pragma warning(pop)

#define MD_MAX_TEXT (8u * 1024u * 1024u)

typedef struct { size_t line, title, length; int level; } MdHeading;
typedef struct { WCHAR fence; size_t count; } MdFence;
typedef struct { size_t start, end, next, content; int heading, code, fence; } MdLine;
static BOOL md_space(WCHAR c) { return c == L' ' || c == L'\t'; }

/* One physical line, with a shared fenced-code state for every consumer. */
static MdLine md_line(const WCHAR *s, size_t start, MdFence *state) {
    MdLine l; size_t p, q, count, indent = 0;
    ZeroMemory(&l, sizeof(l)); l.start = start; l.end = start;
    while (s[l.end] && s[l.end] != L'\r' && s[l.end] != L'\n') ++l.end;
    l.next = l.end;
    if (s[l.next] == L'\r') ++l.next;
    if (s[l.next] == L'\n') ++l.next;
    p = start;
    while (p < l.end && s[p] == L' ' && indent < 4) { ++p; ++indent; }
    l.content = start;
    if (indent <= 3 && (s[p] == L'`' || s[p] == L'~')) {
        WCHAR c = s[p]; q = p;
        while (q < l.end && s[q] == c) ++q;
        count = q - p;
        if (count >= 3) {
            if (state->fence) {
                size_t tail = q; while (tail < l.end && md_space(s[tail])) ++tail;
                if (state->fence == c && count >= state->count && tail == l.end) {
                    state->fence = 0; state->count = 0; l.fence = 1; return l;
                }
            } else {
                BOOL valid = TRUE;
                if (c == L'`') for (; q < l.end; ++q) if (s[q] == L'`') valid = FALSE;
                if (valid) { state->fence = c; state->count = count; l.fence = 1; return l; }
            }
        }
    }
    if (state->fence) { l.code = 1; return l; }
    if (indent <= 3 && s[p] == L'#') {
        q = p; while (q < l.end && s[q] == L'#') ++q;
        count = q - p;
        if (count <= 6 && (q == l.end || md_space(s[q]))) {
            l.heading = (int)count;
            while (q < l.end && md_space(s[q])) ++q;
            l.content = q;
        }
    }
    return l;
}
static size_t md_heading_end(const WCHAR *s, MdLine l) {
    size_t end = l.end, p;
    while (end > l.content && md_space(s[end - 1])) --end;
    p = end; while (p > l.content && s[p - 1] == L'#') --p;
    if (p < end && p > l.content && md_space(s[p - 1])) {
        end = p; while (end > l.content && md_space(s[end - 1])) --end;
    } else if (p == l.content && p < end && p > l.start && md_space(s[p - 1])) end = p;
    return end;
}
static BOOL md_headings(const WCHAR *s, MdHeading **out, size_t *count) {
    size_t pos = 0, n = 0, capacity = 0; MdHeading *items = NULL; MdFence fence = {0};
    *out = NULL; *count = 0;
    while (s[pos]) {
        MdLine l = md_line(s, pos, &fence);
        if (l.heading) {
            if (n == capacity) {
                size_t cap = capacity ? capacity * 2 : 16;
                void *next = items ? HeapReAlloc(GetProcessHeap(), 0, items, cap * sizeof(*items))
                                   : HeapAlloc(GetProcessHeap(), 0, cap * sizeof(*items));
                if (!next) { if (items) HeapFree(GetProcessHeap(), 0, items); return FALSE; }
                items = (MdHeading *)next; capacity = cap;
            }
            items[n].line = l.start; items[n].title = l.content;
            items[n].length = md_heading_end(s, l) - l.content;
            items[n].level = l.heading; ++n;
        }
        pos = l.next;
    }
    *out = items; *count = n; return TRUE;
}
static WCHAR *md_text(HWND edit) {
    int n = GetWindowTextLengthW(edit); WCHAR *s;
    if (n < 0 || (size_t)n > MD_MAX_TEXT) return NULL;
    s = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ((size_t)n + 1) * sizeof(WCHAR));
    if (s) GetWindowTextW(edit, s, n + 1);
    return s;
}
static BOOL md_replace(HWND edit, size_t a, size_t b, const WCHAR *s, size_t n, size_t sa, size_t sb) {
    size_t old = (size_t)GetWindowTextLengthW(edit);
    size_t limit = (size_t)SendMessageW(edit, EM_GETLIMITTEXT, 0, 0);
    if (b > old || a > b || n > MD_MAX_TEXT || old - (b - a) + n > limit) return FALSE;
    SendMessageW(edit, EM_SETSEL, (WPARAM)a, (LPARAM)b);
    SendMessageW(edit, EM_REPLACESEL, TRUE, (LPARAM)s);
    SendMessageW(edit, EM_SETSEL, (WPARAM)sa, (LPARAM)sb);
    SendMessageW(edit, EM_SCROLLCARET, 0, 0); SetFocus(edit); return TRUE;
}
static BOOL md_wrap(HWND edit, const WCHAR *s, size_t a, size_t b, BOOL code) {
    size_t mark = 2, run = 0, longest = 0, pad = 0, n = b - a, i, len;
    WCHAR *out; BOOL ok;
    if (code) {
        for (i = a; i < b; ++i) { run = s[i] == L'`' ? run + 1 : 0; if (run > longest) longest = run; }
        mark = longest + 1;
        if (n && (s[a] == L'`' || s[b - 1] == L'`')) pad = 1;
    }
    len = n + 2 * (mark + pad);
    out = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (len + 1) * sizeof(WCHAR));
    if (!out) return FALSE;
    for (i = 0; i < mark; ++i) out[i] = out[len - i - 1] = code ? L'`' : L'*';
    if (pad) out[mark] = out[len - mark - 1] = L' ';
    memcpy(out + mark + pad, s + a, n * sizeof(WCHAR));
    ok = md_replace(edit, a, b, out, len, a + mark + pad, a + mark + pad + n);
    HeapFree(GetProcessHeap(), 0, out); return ok;
}
/* Returns the content after a native Markdown list marker, or the original indent. */
static size_t md_list_content(const WCHAR *s, size_t p, size_t end) {
    size_t q = p;
    if (q < end && (s[q] == L'-' || s[q] == L'+' || s[q] == L'*')) ++q;
    else {
        size_t digits = 0;
        while (q < end && s[q] >= L'0' && s[q] <= L'9' && digits < 9) { ++q; ++digits; }
        if (!digits || q == end || (s[q] != L'.' && s[q] != L')')) return p;
        ++q;
    }
    if (q == end || !md_space(s[q])) return p;
    while (q < end && md_space(s[q])) ++q;
    return q;
}
static BOOL md_tasks(HWND edit, const WCHAR *s, size_t a, size_t b) {
    size_t start = a, end = b, p, outn = 0, capacity;
    WCHAR *out; BOOL ok;
    while (start && s[start - 1] != L'\n' && s[start - 1] != L'\r') --start;
    if (b > a && b && (s[b - 1] == L'\n' || s[b - 1] == L'\r')) end = b;
    else { while (s[end] && s[end] != L'\r' && s[end] != L'\n') ++end; }
    capacity = (end - start + 1) * 7 + 8;
    out = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, capacity * sizeof(WCHAR));
    if (!out) return FALSE;
    p = start;
    do {
        size_t lineEnd = p, indent, content, k;
        while (lineEnd < end && s[lineEnd] != L'\r' && s[lineEnd] != L'\n') ++lineEnd;
        indent = p; while (indent < lineEnd && md_space(s[indent])) ++indent;
        content = md_list_content(s, indent, lineEnd);
        for (k = p; k < (content == indent ? indent : content); ++k) out[outn++] = s[k];
        if (content == indent) { memcpy(out + outn, L"- ", 2 * sizeof(WCHAR)); outn += 2; }
        if (content + 3 <= lineEnd && s[content] == L'[' && s[content + 2] == L']' &&
            (s[content + 1] == L' ' || s[content + 1] == L'x' || s[content + 1] == L'X') &&
            (content + 3 == lineEnd || md_space(s[content + 3]))) {
            out[outn++] = L'['; out[outn++] = s[content + 1] == L' ' ? L'x' : L' '; out[outn++] = L']';
            content += 3;
        } else { memcpy(out + outn, L"[ ] ", 4 * sizeof(WCHAR)); outn += 4; }
        while (content < lineEnd) out[outn++] = s[content++];
        p = lineEnd;
        if (p < end && s[p] == L'\r') out[outn++] = s[p++];
        if (p < end && s[p] == L'\n') out[outn++] = s[p++];
    } while (p < end);
    out[outn] = 0;
    ok = md_replace(edit, start, end, out, outn, start, start + outn);
    HeapFree(GetProcessHeap(), 0, out); return ok;
}
typedef struct {
    HWND source, owner, child, label;
    WCHAR *snapshot, *scratch;
    MdHeading *headings;
    size_t count;
    BOOL preview;
} MdView;
static HWND md_outline_window, md_preview_window;
static HMODULE md_richedit;
static BOOL md_open(HWND owner, HWND edit, BOOL preview);

static void md_style(HWND rich, int level, BOOL code, BOOL bold) {
    static const LONG sizes[] = {220, 400, 340, 300, 260, 240, 220};
    CHARFORMAT2W cf;
    ZeroMemory(&cf, sizeof(cf)); cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_FACE | CFM_SIZE | CFM_BOLD | CFM_ITALIC | CFM_COLOR | CFM_BACKCOLOR | CFM_STRIKEOUT;
    cf.dwEffects = (level || bold) ? CFE_BOLD : 0;
    cf.yHeight = code ? 200 : sizes[level];
    cf.crTextColor = code ? RGB(55, 65, 80) : RGB(25, 30, 38);
    cf.crBackColor = code ? RGB(240, 242, 245) : RGB(255, 255, 255);
    wcscpy_s(cf.szFaceName, LF_FACESIZE, code ? L"Consolas" : L"Segoe UI");
    SendMessageW(rich, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
}
static void md_emit(MdView *v, const WCHAR *s, size_t n, int heading, BOOL code, BOOL bold) {
    if (!n) return;
    memcpy(v->scratch, s, n * sizeof(WCHAR)); v->scratch[n] = 0;
    md_style(v->child, heading, code, bold);
    SendMessageW(v->child, EM_REPLACESEL, FALSE, (LPARAM)v->scratch);
}
/* Deliberately conservative inline subset, never interpreted as RTF/HTML. */
static void md_inline(MdView *v, const WCHAR *s, size_t a, size_t b, int heading) {
    size_t p = a, plain = a;
    while (p < b) {
        size_t mark = 0, q, close = b; BOOL code = s[p] == L'`';
        if (code) { while (p + mark < b && s[p + mark] == L'`') ++mark; }
        else if (s[p] == L'*' && p + 1 < b && s[p + 1] == L'*') mark = 2;
        if (!mark) { ++p; continue; }
        q = p + mark;
        while (q < b) {
            size_t run = 0;
            if (code && s[q] == L'`') {
                while (q + run < b && s[q + run] == L'`') ++run;
                if (run == mark) { close = q; break; }
                q += run;
            } else if (!code && q + 1 < b && s[q] == L'*' && s[q + 1] == L'*') { close = q; break; }
            else ++q;
        }
        if (close == b) break;
        md_emit(v, s + plain, p - plain, heading, FALSE, FALSE);
        a = p + mark; q = close;
        if (code && q > a + 1 && s[a] == L' ' && s[q - 1] == L' ') {
            size_t k = a; while (k < q && s[k] == L' ') ++k;
            if (k < q) { ++a; --q; }
        }
        md_emit(v, s + a, q - a, heading, code, !code);
        p = close + mark; plain = p;
    }
    md_emit(v, s + plain, b - plain, heading, FALSE, FALSE);
}
static void md_render(MdView *v) {
    size_t pos = 0; MdFence fence = {0}; const WCHAR *s = v->snapshot;
    SendMessageW(v->child, WM_SETREDRAW, FALSE, 0);
    SendMessageW(v->child, EM_EXLIMITTEXT, 0, MD_MAX_TEXT * 2);
    SendMessageW(v->child, EM_AUTOURLDETECT, FALSE, 0);
    SendMessageW(v->child, EM_SETBKGNDCOLOR, 0, RGB(255, 255, 255));
    while (s[pos]) {
        MdLine l = md_line(s, pos, &fence); size_t p = l.content, end = l.end, indent = 0, content;
        PARAFORMAT2 pf; ZeroMemory(&pf, sizeof(pf)); pf.cbSize = sizeof(pf);
        pos = l.next; if (l.fence) continue;
        pf.dwMask = PFM_STARTINDENT | PFM_OFFSET | PFM_SPACEBEFORE | PFM_SPACEAFTER;
        pf.dySpaceBefore = l.heading ? 160 : 0; pf.dySpaceAfter = l.code ? 0 : 80;
        if (l.heading) end = md_heading_end(s, l);
        if (l.code) pf.dxStartIndent = 240;
        else if (!l.heading) {
            while (p < end && md_space(s[p])) { indent += s[p] == L'\t' ? 4 : 1; ++p; }
            content = md_list_content(s, p, end);
            if (content != p) {
                pf.dxStartIndent = (LONG)(240 + (indent > 40 ? 40 : indent) * 100); pf.dxOffset = -180;
                SendMessageW(v->child, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
                if (content + 3 <= end && s[content] == L'[' && s[content + 2] == L']' &&
                    (s[content + 1] == L' ' || s[content + 1] == L'x' || s[content + 1] == L'X') &&
                    (content + 3 == end || md_space(s[content + 3]))) {
                    md_emit(v, s[content + 1] == L' ' ? L"☐ " : L"☑ ", 2, 0, FALSE, FALSE);
                    p = content + 3; while (p < end && md_space(s[p])) ++p;
                } else {
                    if (s[p] >= L'0' && s[p] <= L'9') md_emit(v, s + p, content - p, 0, FALSE, FALSE);
                    else md_emit(v, L"• ", 2, 0, FALSE, FALSE);
                    p = content;
                }
            } else p = l.start;
        }
        SendMessageW(v->child, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
        if (l.code) md_emit(v, s + l.start, end - l.start, 0, TRUE, FALSE);
        else md_inline(v, s, p, end, l.heading);
        md_emit(v, L"\r", 1, l.heading, l.code, FALSE);
    }
    SendMessageW(v->child, EM_SETSEL, 0, 0);
    SendMessageW(v->child, EM_SETREADONLY, TRUE, 0);
    SendMessageW(v->child, EM_EMPTYUNDOBUFFER, 0, 0);
    SendMessageW(v->child, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(v->child, NULL, TRUE);
}
static void md_jump(HWND window, MdView *v) {
    LRESULT index; WCHAR *now;
    if (!IsWindow(v->source)) { DestroyWindow(window); return; }
    now = md_text(v->source); if (!now) return;
    if (wcscmp(now, v->snapshot)) {
        HWND owner = v->owner, source = v->source;
        HeapFree(GetProcessHeap(), 0, now);
        if (md_open(owner, source, FALSE)) {
            MdView *fresh = (MdView *)GetWindowLongPtrW(md_outline_window, GWLP_USERDATA);
            if (fresh) SetWindowTextW(fresh->label, L"Source changed: refreshed. Choose a heading, then Enter.");
        }
        return;
    }
    HeapFree(GetProcessHeap(), 0, now);
    index = SendMessageW(v->child, LB_GETCURSEL, 0, 0);
    if (index != LB_ERR && (size_t)index < v->count) {
        HWND source = v->source; size_t position = v->headings[index].line;
        DestroyWindow(window); SetForegroundWindow(GetAncestor(source, GA_ROOT)); SetFocus(source);
        SendMessageW(source, EM_SETSEL, (WPARAM)position, (LPARAM)position);
        SendMessageW(source, EM_SCROLLCARET, 0, 0);
    }
}
static LRESULT CALLBACK md_child_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                      UINT_PTR subclass, DWORD_PTR ref) {
    HWND window = (HWND)ref; MdView *v = (MdView *)GetWindowLongPtrW(window, GWLP_USERDATA);
    (void)subclass;
    if (v && msg == WM_KEYDOWN) {
        if (wp == VK_ESCAPE) {
            HWND source = v->source; DestroyWindow(window); if (IsWindow(source)) SetFocus(source); return 0;
        }
        if (!v->preview && wp == VK_RETURN) { md_jump(window, v); return 0; }
        if (wp == VK_F6 || (wp == 'R' && (GetKeyState(VK_CONTROL) & 0x8000))) {
            md_open(v->owner, v->source, v->preview); return 0;
        }
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, md_child_proc, 1);
    return DefSubclassProc(hwnd, msg, wp, lp);
}
static void md_free_view(MdView *v) {
    if (v->snapshot) HeapFree(GetProcessHeap(), 0, v->snapshot);
    if (v->scratch) HeapFree(GetProcessHeap(), 0, v->scratch);
    if (v->headings) HeapFree(GetProcessHeap(), 0, v->headings);
    HeapFree(GetProcessHeap(), 0, v);
}
static LRESULT CALLBACK md_window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MdView *v = (MdView *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (v) switch (msg) {
        case WM_SIZE: {
            RECT r; GetClientRect(hwnd, &r);
            MoveWindow(v->label, 12, 8, r.right > 24 ? r.right - 24 : 1, 34, TRUE);
            MoveWindow(v->child, 12, 46, r.right > 24 ? r.right - 24 : 1, r.bottom > 58 ? r.bottom - 58 : 1, TRUE);
            return 0;
        }
        case WM_SETFOCUS: SetFocus(v->child); return 0;
        case WM_COMMAND:
            if (!v->preview && LOWORD(wp) == 1 && HIWORD(wp) == LBN_DBLCLK) { md_jump(hwnd, v); return 0; }
            break;
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
        case WM_NCDESTROY:
            if (md_outline_window == hwnd) md_outline_window = NULL;
            if (md_preview_window == hwnd) md_preview_window = NULL;
            md_free_view(v);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0); break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
static BOOL md_open(HWND owner, HWND edit, BOOL preview) {
    WNDCLASSW wc; MdView *v; HWND window; HINSTANCE instance = GetModuleHandleW(NULL);
    HWND previous = preview ? md_preview_window : md_outline_window;
    size_t i, length; RECT r = {100, 100, 850, 720}; DWORD caret = 0; size_t selected = 0;
    if (!IsWindow(owner) || !IsWindow(edit)) return FALSE;
    if (preview && !md_richedit) {
        /* System-directory-only DLL loading, no search of document/current directory. */
        md_richedit = LoadLibraryExW(L"Msftedit.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!md_richedit) return FALSE;
    }
    ZeroMemory(&wc, sizeof(wc)); wc.lpfnWndProc = md_window_proc; wc.hInstance = instance;
    wc.lpszClassName = L"NoteChad.Markdown"; wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return FALSE;
    v = (MdView *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*v)); if (!v) return FALSE;
    v->source = edit; v->owner = owner; v->preview = preview; v->snapshot = md_text(edit);
    if (!v->snapshot) { HeapFree(GetProcessHeap(), 0, v); return FALSE; }
    length = wcslen(v->snapshot);
    if (preview) v->scratch = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (length + 8) * sizeof(WCHAR));
    if ((preview && !v->scratch) || (!preview && !md_headings(v->snapshot, &v->headings, &v->count))) {
        md_free_view(v); return FALSE;
    }
    GetWindowRect(owner, &r);
    window = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName,
        preview ? L"Markdown reading preview — snapshot" : L"Markdown headings",
        WS_OVERLAPPEDWINDOW, r.left + 35, r.top + 35, 760, 640, owner, NULL, instance, NULL);
    if (!window) { md_free_view(v); return FALSE; }
    SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)v);
    v->label = CreateWindowW(L"STATIC", preview ? L"Read-only snapshot • F6 / Ctrl+R refresh • Esc returns to editor"
        : L"↑ / ↓ choose • Enter / double-click jumps • Ctrl+R refresh • Esc closes", WS_CHILD | WS_VISIBLE,
        0, 0, 1, 1, window, (HMENU)2, instance, NULL);
    v->child = CreateWindowExW(WS_EX_CLIENTEDGE, preview ? MSFTEDIT_CLASS : L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP |
        (preview ? (ES_MULTILINE | ES_AUTOVSCROLL | ES_NOHIDESEL) : (LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_HSCROLL)),
        0, 0, 1, 1, window, (HMENU)1, instance, NULL);
    if (!v->label || !v->child || !SetWindowSubclass(v->child, md_child_proc, 1, (DWORD_PTR)window)) {
        DestroyWindow(window); return FALSE;
    }
    SendMessageW(v->label, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    SendMessageW(v->child, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    if (preview) md_render(v);
    else {
        SendMessageW(edit, EM_GETSEL, (WPARAM)&caret, 0);
        for (i = 0; i < v->count; ++i) {
            WCHAR label[560]; size_t k = 0, t, take = v->headings[i].length;
            for (t = 1; t < (size_t)v->headings[i].level; ++t) { label[k++] = L' '; label[k++] = L' '; }
            if (take > 500) take = 500;
            if (take) { memcpy(label + k, v->snapshot + v->headings[i].title, take * sizeof(WCHAR)); k += take; }
            else { memcpy(label + k, L"(untitled)", 10 * sizeof(WCHAR)); k += 10; }
            if (take < v->headings[i].length) label[k++] = L'…'; label[k] = 0;
            if (SendMessageW(v->child, LB_ADDSTRING, 0, (LPARAM)label) < 0) { DestroyWindow(window); return FALSE; }
            if (v->headings[i].line <= caret) selected = i;
        }
        if (v->count) SendMessageW(v->child, LB_SETCURSEL, (WPARAM)selected, 0);
        else SetWindowTextW(v->label, L"No ATX headings (# through ######) outside fenced code. Esc closes.");
        SendMessageW(v->child, LB_SETHORIZONTALEXTENT, 3600, 0);
    }
    if (IsWindow(previous)) DestroyWindow(previous);
    if (preview) md_preview_window = window; else md_outline_window = window;
    SendMessageW(window, WM_SIZE, 0, 0); ShowWindow(window, SW_SHOW); SetForegroundWindow(window); SetFocus(v->child);
    return TRUE;
}
void md_forget(HWND edit) {
    HWND windows[2] = {md_outline_window, md_preview_window};
    for (int i = 0; i < 2; ++i) if (IsWindow(windows[i])) {
        MdView *v = (MdView *)GetWindowLongPtrW(windows[i], GWLP_USERDATA);
        if (v && v->source == edit) DestroyWindow(windows[i]);
    }
}
BOOL md_command(HWND owner, HWND edit, UINT id) {
    WCHAR *s; DWORD a = 0, b = 0; BOOL ok;
    if (!IsWindow(edit)) return FALSE;
    if (id == MD_HEADING_NAVIGATION || id == MD_READING_PREVIEW)
        return md_open(owner, edit, id == MD_READING_PREVIEW);
    if (id < MD_BOLD || id > MD_TASK) return FALSE;
    if (GetWindowLongPtrW(edit, GWL_STYLE) & ES_READONLY) return FALSE;
    s = md_text(edit); if (!s) return FALSE;
    SendMessageW(edit, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
    ok = id == MD_TASK ? md_tasks(edit, s, a, b) : md_wrap(edit, s, a, b, id == MD_INLINE_CODE);
    HeapFree(GetProcessHeap(), 0, s); return ok;
}
