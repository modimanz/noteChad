/* Isolated native tests: private parser plus real classic EDIT commands. */
#include <assert.h>
#include <stdio.h>
#include <wchar.h>
#include "../src/markdown.c"

static void test_headings(void) {
    const WCHAR *text = L"# One\r\n## Two ##\n### Three\n#### Four\n##### Five\n###### Six\n####### no\n#no\n    # indented\n```md\n# hidden\n~~~\n## also hidden\n```\n~~~\n# tilde hidden\n~~~~\n   # Last\n";
    MdHeading *h = NULL; size_t n = 0;
    assert(md_headings(text, &h, &n));
    assert(n == 7);
    for (size_t i = 0; i < 6; ++i) assert(h[i].level == (int)i + 1);
    assert(h[1].length == 3 && wcsncmp(text + h[1].title, L"Two", 3) == 0);
    assert(wcsncmp(text + h[6].title, L"Last", 4) == 0);
    HeapFree(GetProcessHeap(), 0, h);
}
static void expect_text(HWND edit, const WCHAR *want) {
    WCHAR got[1024]; GetWindowTextW(edit, got, 1024);
    if (wcscmp(got, want)) { fwprintf(stderr, L"expected <%ls> got <%ls>\n", want, got); fflush(stderr); }
    assert(wcscmp(got, want) == 0);
}
static void test_edit_commands(void) {
    HWND owner = CreateWindowW(L"STATIC", L"test", WS_OVERLAPPED, 0, 0, 400, 400, NULL, NULL, NULL, NULL);
    HWND edit = CreateWindowW(L"EDIT", L"αβ", WS_CHILD | ES_MULTILINE, 0, 0, 300, 300, owner, NULL, NULL, NULL);
    DWORD a, b; assert(owner && edit);
    SetWindowTextW(edit, L"αβ");
    SendMessageW(edit, EM_SETSEL, 0, 2);
    assert(md_command(owner, edit, MD_BOLD)); expect_text(edit, L"**αβ**");
    SendMessageW(edit, EM_GETSEL, (WPARAM)&a, (LPARAM)&b); assert(a == 2 && b == 4);
    assert(SendMessageW(edit, EM_CANUNDO, 0, 0));
    SetWindowTextW(edit, L"a`b"); SendMessageW(edit, EM_SETSEL, 0, 3);
    assert(md_command(owner, edit, MD_INLINE_CODE)); expect_text(edit, L"``a`b``");
    SetWindowTextW(edit, L""); assert(md_command(owner, edit, MD_INLINE_CODE)); expect_text(edit, L"``");
    SendMessageW(edit, EM_GETSEL, (WPARAM)&a, (LPARAM)&b); assert(a == 1 && b == 1);
    SetWindowTextW(edit, L"first\r\n- second\r\nlast"); SendMessageW(edit, EM_SETSEL, 0, 17);
    assert(md_command(owner, edit, MD_TASK)); expect_text(edit, L"- [ ] first\r\n- [ ] second\r\nlast");
    assert(md_command(owner, edit, MD_TASK)); expect_text(edit, L"- [x] first\r\n- [x] second\r\nlast");
    assert(md_command(owner, edit, MD_TASK)); expect_text(edit, L"- [ ] first\r\n- [ ] second\r\nlast");
    SendMessageW(edit, EM_SETREADONLY, TRUE, 0); assert(!md_command(owner, edit, MD_BOLD));
    assert(!md_command(owner, edit, 999));
    DestroyWindow(owner);
}
static void test_native_views(void) {
    const WCHAR *doc = L"# One\r\n- [x] done\r\n~~~\r\n# not heading\r\n~~~\r\n## Two";
    HWND owner = CreateWindowW(L"STATIC", L"view test", WS_OVERLAPPED, 0, 0, 400, 400, NULL, NULL, NULL, NULL);
    HWND edit = CreateWindowW(L"EDIT", L"", WS_CHILD | ES_MULTILINE, 0, 0, 300, 300, owner, NULL, NULL, NULL);
    HWND view, child; DWORD a, b; WCHAR rendered[1024]; CHARFORMAT2W cf;
    SetWindowTextW(edit, doc); SendMessageW(edit, EM_SETMODIFY, FALSE, 0); SendMessageW(edit, EM_SETSEL, 2, 4);
    assert(md_command(owner, edit, MD_HEADING_NAVIGATION));
    view = md_outline_window; assert(view);
    child = GetDlgItem(view, 1); assert(SendMessageW(child, LB_GETCOUNT, 0, 0) == 2);
    SendMessageW(child, LB_SETCURSEL, 1, 0);
    SendMessageW(view, WM_COMMAND, MAKEWPARAM(1, LBN_DBLCLK), (LPARAM)child);
    SendMessageW(edit, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
    assert(a == (DWORD)(wcsstr(doc, L"## Two") - doc) && a == b);
    assert(!md_command(owner, edit, 305));
    child = md_preview_create(owner); assert(child);
    assert(GetParent(child) == owner && (GetWindowLongPtrW(child, GWL_STYLE) & WS_CHILD));
    assert(!(GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE));
    assert(md_preview_update(child, edit, FALSE));
    assert(!(GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE));
    assert(GetWindowLongPtrW(child, GWL_STYLE) & ES_READONLY);
    GetWindowTextW(child, rendered, 1024);
    assert(wcsstr(rendered, L"One") && !wcsstr(rendered, L"# One"));
    assert(wcsstr(rendered, L"☑ done") && wcsstr(rendered, L"# not heading"));
    ZeroMemory(&cf, sizeof(cf)); cf.cbSize = sizeof(cf); SendMessageW(child, EM_SETSEL, 0, 3);
    SendMessageW(child, EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf); assert(cf.yHeight > 220);
    expect_text(edit, doc); assert(!SendMessageW(edit, EM_GETMODIFY, 0, 0));
    SetWindowTextW(edit, L"# Refreshed"); assert(md_preview_update(child, edit, TRUE));
    GetWindowTextW(child, rendered, 1024); assert(wcsstr(rendered, L"Refreshed"));
    SendMessageW(child, WM_KEYDOWN, VK_ESCAPE, 0); assert(IsWindow(child));
    DestroyWindow(owner); assert(!IsWindow(child));
}
static void test_edge_cases(void) {
    MdHeading *h = NULL; size_t count;
    const WCHAR *doc = L"#\n## ###\n``` bad`info\n### Valid\n````\n# hidden\n```\n## hidden too\n`````\n# Final#\n";
    HWND owner, edit, view, child; DWORD a = 0, b = 0;
    assert(md_headings(doc, &h, &count)); assert(count == 4);
    assert(h[0].length == 0 && h[1].length == 0 && h[2].level == 3);
    assert(h[3].length == 6); HeapFree(GetProcessHeap(), 0, h);
    owner = CreateWindowW(L"STATIC", L"edge tests", WS_OVERLAPPED, 0, 0, 400, 400, NULL, NULL, NULL, NULL);
    edit = CreateWindowW(L"EDIT", L"", WS_CHILD | ES_MULTILINE, 0, 0, 300, 300, owner, NULL, NULL, NULL);
    SetWindowTextW(edit, L"`ends`"); SendMessageW(edit, EM_SETSEL, 0, 6);
    assert(md_command(owner, edit, MD_INLINE_CODE)); expect_text(edit, L"`` `ends` ``");
    SetWindowTextW(edit, L"1. numbered\n  + [X] checked\n\n"); SendMessageW(edit, EM_SETSEL, 0, 25);
    assert(md_command(owner, edit, MD_TASK)); expect_text(edit, L"1. [ ] numbered\n  + [ ] checked\n\n");
    SetWindowTextW(edit, L"abc"); SendMessageW(edit, EM_SETSEL, 0, 3); SendMessageW(edit, EM_SETLIMITTEXT, 4, 0);
    assert(!md_command(owner, edit, MD_BOLD)); expect_text(edit, L"abc");
    SendMessageW(edit, EM_SETLIMITTEXT, 100000, 0);
    SetWindowTextW(edit, L"# old"); assert(md_command(owner, edit, MD_HEADING_NAVIGATION));
    view = md_outline_window; child = GetDlgItem(view, 1);
    SetWindowTextW(edit, L"prefix\r\n## New"); SendMessageW(edit, EM_SETSEL, 1, 1);
    SendMessageW(child, WM_KEYDOWN, VK_RETURN, 0);
    SendMessageW(edit, EM_GETSEL, (WPARAM)&a, (LPARAM)&b); assert(a == 1 && b == 1);
    view = md_outline_window; child = GetDlgItem(view, 1);
    SendMessageW(child, WM_KEYDOWN, VK_RETURN, 0);
    SendMessageW(edit, EM_GETSEL, (WPARAM)&a, (LPARAM)&b); assert(a == 8 && b == 8);
    SetWindowTextW(edit, L"# headings\r\n**bold** and ``code``\r\n- list\r\n1. ordered\r\n☃ literal <script>no</script> https://example.invalid\r\n");
    child = md_preview_create(owner); assert(child); assert(md_preview_update(child, edit, FALSE));
    { WCHAR got[1024]; CHARFORMAT2W cf;
      GetWindowTextW(child, got, 1024); assert(wcsstr(got, L"bold and code"));
      assert(wcsstr(got, L"• list") && wcsstr(got, L"1. ordered") && wcsstr(got, L"<script>no</script>"));
      ZeroMemory(&cf, sizeof(cf)); cf.cbSize = sizeof(cf);
      { FINDTEXTEXW find; ZeroMemory(&find, sizeof(find)); find.chrg.cpMax = -1; find.lpstrText = L"code";
        assert(SendMessageW(child, EM_FINDTEXTEXW, FR_DOWN, (LPARAM)&find) >= 0);
        SendMessageW(child, EM_EXSETSEL, 0, (LPARAM)&find.chrgText); }
      SendMessageW(child, EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf); assert(wcscmp(cf.szFaceName, L"Consolas") == 0);
    }
    DestroyWindow(owner); assert(!IsWindow(child));
}
static void test_long_offsets_and_lifecycle(void) {
    HWND owner = CreateWindowW(L"STATIC", L"long tests", WS_OVERLAPPED, 0, 0, 400, 400, NULL, NULL, NULL, NULL);
    HWND edit = CreateWindowW(L"EDIT", L"", WS_CHILD | ES_MULTILINE, 0, 0, 300, 300, owner, NULL, NULL, NULL);
    WCHAR *doc = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 70032 * sizeof(WCHAR));
    DWORD a = 0, b = 0; HWND view, child;
    assert(doc); for (size_t i = 0; i < 70000; ++i) doc[i] = L'a';
    wcscpy_s(doc + 70000, 32, L"\r\n###### Far heading");
    SendMessageW(edit, EM_SETLIMITTEXT, 200000, 0); SetWindowTextW(edit, doc);
    SendMessageW(edit, EM_SETSEL, 70002, 70002);
    assert(md_command(owner, edit, MD_HEADING_NAVIGATION));
    view = md_outline_window; child = GetDlgItem(view, 1);
    assert(SendMessageW(child, LB_GETCOUNT, 0, 0) == 1); SendMessageW(child, WM_KEYDOWN, VK_RETURN, 0);
    SendMessageW(edit, EM_GETSEL, (WPARAM)&a, (LPARAM)&b); assert(a == 70002 && b == 70002);
    SendMessageW(edit, EM_SETSEL, 69999, 70000); assert(md_command(owner, edit, MD_BOLD));
    SendMessageW(edit, EM_GETSEL, (WPARAM)&a, (LPARAM)&b); assert(a == 70001 && b == 70002);
    HeapFree(GetProcessHeap(), 0, doc);
    SetWindowTextW(edit, L""); child = md_preview_create(owner); assert(child);
    for (int i = 0; i < 12; ++i) {
        assert(md_preview_update(child, edit, TRUE)); assert(GetWindowTextLengthW(child) == 0);
    }
    DestroyWindow(owner); assert(!IsWindow(child) && !md_outline_window);
}
static LRESULT CALLBACK test_drop_insert(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                         UINT_PTR id, DWORD_PTR ref) {
    (void)id;
    if (msg == (UINT)ref) return 0;
    return DefSubclassProc(hwnd, msg, wp, lp);
}
static void test_preview_contract(void) {
    HWND owner = CreateWindowW(L"STATIC", L"pane contract", WS_OVERLAPPED, 0, 0, 500, 400, NULL, NULL, NULL, NULL);
    HWND edit = CreateWindowW(L"EDIT", L"", WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL, 0, 0, 300, 300, owner, NULL, NULL, NULL);
    HWND pane, focus; DWORD a = 0, b = 0; CHARRANGE range; POINT before, after;
    WCHAR *doc = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (MD_PREVIEW_MAX_TEXT + 2) * sizeof(WCHAR));
    WCHAR selected[32]; size_t p = 0; LRESULT undo; LARGE_INTEGER start, end, freq;
    assert(owner && edit && doc); assert(!md_preview_create(NULL));
    focus = GetFocus(); pane = md_preview_create(owner); assert(pane && GetFocus() == focus);
    assert(LOWORD(SendMessageW(pane, EM_GETMARGINS, 0, 0)) >= 8);
    assert(!md_preview_update(edit, edit, FALSE));
    MoveWindow(pane, 0, 0, 400, 200, FALSE); ShowWindow(pane, SW_SHOWNOACTIVATE);
    for (int i = 0; i < 100; ++i) p += (size_t)swprintf_s(doc + p, MD_PREVIEW_MAX_TEXT + 2 - p, L"Line %d **bold** and `code`\r\n", i);
    SetWindowTextW(edit, doc); SendMessageW(edit, EM_SETSEL, 0, 4);
    SendMessageW(edit, EM_REPLACESEL, TRUE, (LPARAM)L"unsaved ");
    SendMessageW(edit, EM_SETSEL, 3, 9); undo = SendMessageW(edit, EM_CANUNDO, 0, 0);
    assert(undo);
    assert(SendMessageW(edit, EM_GETMODIFY, 0, 0));
    assert(md_preview_update(pane, edit, FALSE)); assert(GetFocus() == focus);
    assert(GetWindowLongPtrW(pane, GWL_STYLE) & WS_VISIBLE);
    SendMessageW(edit, EM_GETSEL, (WPARAM)&a, (LPARAM)&b); assert(a == 3 && b == 9);
    assert(SendMessageW(edit, EM_CANUNDO, 0, 0) == undo && SendMessageW(edit, EM_GETMODIFY, 0, 0));
    SendMessageW(pane, EM_SETSEL, 0, 7);
    SendMessageW(pane, EM_GETSELTEXT, 0, (LPARAM)selected); assert(wcscmp(selected, L"unsaved") == 0);
    SendMessageW(pane, WM_CHAR, 'X', 0);
    SendMessageW(pane, EM_GETSELTEXT, 0, (LPARAM)selected); assert(wcscmp(selected, L"unsaved") == 0);
    SendMessageW(pane, EM_LINESCROLL, 0, 30); SendMessageW(pane, EM_GETSCROLLPOS, 0, (LPARAM)&before);
    assert(before.y > 0); assert(md_preview_update(pane, edit, TRUE));
    SendMessageW(pane, EM_EXGETSEL, 0, (LPARAM)&range); assert(range.cpMin == 0 && range.cpMax == 7);
    SendMessageW(pane, EM_GETSCROLLPOS, 0, (LPARAM)&after); assert(after.y == before.y);
    assert(md_preview_update(pane, edit, FALSE));
    SendMessageW(pane, EM_EXGETSEL, 0, (LPARAM)&range); assert(range.cpMin == 0 && range.cpMax == 0);
    SendMessageW(pane, EM_GETSCROLLPOS, 0, (LPARAM)&after); assert(after.y == 0);
    assert(SendMessageW(edit, EM_UNDO, 0, 0));
    { WCHAR original[4096]; GetWindowTextW(edit, original, 4096); assert(wcscmp(original, doc) == 0); }
    SetWindowTextW(edit, L"short"); SendMessageW(pane, EM_SETSEL, 100, 110);
    assert(md_preview_update(pane, edit, TRUE));
    SendMessageW(pane, EM_EXGETSEL, 0, (LPARAM)&range);
    assert(range.cpMin <= GetWindowTextLengthW(pane) && range.cpMax <= GetWindowTextLengthW(pane));
    /* Refuse aliasing without replacing the source itself. */
    assert(!md_preview_update(pane, pane, TRUE)); expect_text(pane, L"short\r\n");
    assert(!md_preview_update(pane, NULL, FALSE));
    { WCHAR got[512]; GetWindowTextW(pane, got, 512); assert(wcsstr(got, L"unavailable")); }
    for (size_t i = 0; i < MD_PREVIEW_MAX_TEXT + 1; ++i) doc[i] = L'a';
    doc[MD_PREVIEW_MAX_TEXT + 1] = 0;
    SendMessageW(edit, EM_SETLIMITTEXT, MD_PREVIEW_MAX_TEXT + 2, 0); SetWindowTextW(edit, doc);
    QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&start);
    assert(!md_preview_update(pane, edit, TRUE)); QueryPerformanceCounter(&end);
    assert((end.QuadPart - start.QuadPart) < freq.QuadPart * 2);
    assert(GetWindowTextLengthW(edit) == MD_PREVIEW_MAX_TEXT + 1);
    { WCHAR got[512]; GetWindowTextW(pane, got, 512); assert(wcsstr(got, L"limited")); }
    for (size_t i = 0; i < MD_PREVIEW_MAX_LINES + 1; ++i) { doc[2 * i] = L'x'; doc[2 * i + 1] = L'\n'; }
    doc[2 * (MD_PREVIEW_MAX_LINES + 1)] = 0; SetWindowTextW(edit, doc);
    assert(!md_preview_update(pane, edit, FALSE));
    /* Dense inline formatting is independently bounded even below the text cap. */
    p = 0;
    for (size_t i = 0; i < 3000; ++i) { memcpy(doc + p, L"**x** ", 6 * sizeof(WCHAR)); p += 6; }
    doc[p] = 0; SetWindowTextW(edit, doc);
    QueryPerformanceCounter(&start); assert(!md_preview_update(pane, edit, FALSE));
    QueryPerformanceCounter(&end); assert((end.QuadPart - start.QuadPart) < freq.QuadPart * 2);
    { WCHAR got[512]; GetWindowTextW(pane, got, 512); assert(wcsstr(got, L"limited")); }
    /* Simulate a rejected native insertion; never leave a plausible partial render. */
    SetWindowTextW(edit, L"**recoverable**");
    assert(SetWindowSubclass(pane, test_drop_insert, 2, EM_REPLACESEL));
    assert(!md_preview_update(pane, edit, FALSE));
    { WCHAR got[512]; GetWindowTextW(pane, got, 512); assert(wcsstr(got, L"unavailable")); }
    assert(RemoveWindowSubclass(pane, test_drop_insert, 2));
    assert(SetWindowSubclass(pane, test_drop_insert, 2, EM_SETCHARFORMAT));
    assert(!md_preview_update(pane, edit, FALSE));
    assert(RemoveWindowSubclass(pane, test_drop_insert, 2));
    assert(SetWindowSubclass(pane, test_drop_insert, 2, EM_SETPARAFORMAT));
    assert(!md_preview_update(pane, edit, FALSE));
    assert(RemoveWindowSubclass(pane, test_drop_insert, 2));
    assert(md_preview_update(pane, edit, FALSE)); expect_text(pane, L"recoverable\r\n");
    assert(GetWindowLongPtrW(pane, GWL_STYLE) & ES_READONLY);
    assert(!SendMessageW(pane, EM_CANUNDO, 0, 0));
    md_forget(edit); assert(IsWindow(pane));
    HeapFree(GetProcessHeap(), 0, doc); DestroyWindow(owner); assert(!IsWindow(pane));
    assert(!md_preview_update(pane, NULL, FALSE));
}
int main(void) {
    test_preview_contract();
    puts("PASS pane contract: focus, margins, selection/scroll, unsaved source/undo, read-only selection, bounds, failure recovery");
    test_long_offsets_and_lifecycle();
    puts("PASS long offsets/lifecycle: >64K source positions, empty snapshot, repeated refresh, owner cleanup");
    test_edge_cases();
    puts("PASS edges: empty headings, longer fences, list preservation, limits, stale-outline guard, inline styles");
    test_headings();
    test_edit_commands();
    test_native_views();
    puts("PASS native views: outline jump, direct hidden pane, styled snapshot, tasks/code, immutable source, refresh, caller-owned Escape");
    puts("PASS classic EDIT: Unicode wrap, selection, undo, adaptive backticks, tasks, readonly");
    puts("PASS headings: six levels, closing hashes, indentation, mixed fences");
    return 0;
}
