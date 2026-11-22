#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <stdio.h>
#include "drag.h"

static int failures, checks, drops, original;
static HWND frame_a, frame_b, strip_a, strip_b;
static TabDrop plan;
static LPARAM last_token;
static BOOL received, native_focus, foreign_instance;
typedef struct { DWORD pid; HWND window; } FindProcessWindow;
static BOOL CALLBACK find_process_window(HWND window, LPARAM data)
{
    FindProcessWindow *find = (FindProcessWindow *)data;
    DWORD pid;
    GetWindowThreadProcessId(window, &pid);
    if (pid == find->pid && IsWindowVisible(window)) {
        find->window = window;
        return FALSE;
    }
    return TRUE;
}

#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

static LRESULT CALLBACK frame_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_NC_TABDROP) {
        TabDrop sentinel = { (HWND)(UINT_PTR)123, -99, { -9, -9 } };
        ++drops;
        original = (int)w;
        last_token = l;
        CHECK(!drag_take(h == frame_a ? frame_b : frame_a, l, &sentinel));
        CHECK(!drag_take(h == frame_a ? frame_b : frame_a, l, (TabDrop *)(UINT_PTR)1));
        CHECK(!drag_take(h, l ^ (LPARAM)((UINT_PTR)1 << 40), (TabDrop *)(UINT_PTR)1));
        CHECK(sentinel.index == -99);
        CHECK(!drag_take(h, l ^ (LPARAM)((UINT_PTR)1 << 40), &sentinel));
        CHECK(!drag_take(h, l, NULL));
        received = drag_take(h, l, &plan);
        CHECK(received);
        CHECK(!drag_take(h, l, &sentinel));
        return 0;
    }
    if (foreign_instance && m == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(h, m, w, l);
}

static HWND make_frame(const WCHAR *class_name, int x, int y)
{
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW, class_name, L"Drag fixture",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, x, y, 400, 220,
        NULL, NULL, GetModuleHandleW(NULL), NULL);
    CHECK(h != NULL);
    return h;
}

static HWND make_strip(HWND frame)
{
    int i;
    HWND h = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        0, 0, 370, 50, frame, NULL, GetModuleHandleW(NULL), NULL);
    CHECK(h != NULL);
    for (i = 0; i < 3; ++i) {
        TCITEMW item = {0};
        item.mask = TCIF_TEXT;
        item.pszText = i == 0 ? L"Alpha tab" : i == 1 ? L"Beta tab" : L"Gamma tab";
        CHECK(TabCtrl_InsertItem(h, i, &item) == i);
    }
    return h;
}

static POINT tab_point(HWND h, int i)
{
    RECT r;
    POINT p;
    CHECK(TabCtrl_GetItemRect(h, i, &r));
    p.x = (r.left + r.right) / 2;
    p.y = (r.top + r.bottom) / 2;
    ClientToScreen(h, &p);
    return p;
}

static void mouse(HWND h, UINT msg, WPARAM keys, POINT screen)
{
    POINT local = screen;
    ScreenToClient(h, &local);
    SendMessageW(h, msg, keys, MAKELPARAM(local.x, local.y));
}

static void gesture(POINT from, POINT to)
{
    mouse(strip_a, WM_LBUTTONDOWN, MK_LBUTTON, from);
    mouse(strip_a, WM_MOUSEMOVE, MK_LBUTTON, to);
    mouse(strip_a, WM_LBUTTONUP, 0, to);
}

int main(int argc, char **argv)
{
    INITCOMMONCONTROLSEX ic = { sizeof(ic), ICC_TAB_CLASSES };
    WNDCLASSW wc = {0};
    POINT a, b;
    TabDrop unused;
    InitCommonControlsEx(&ic);
    wc.lpfnWndProc = frame_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"NoteChad.Editor";
    CHECK(RegisterClassW(&wc) != 0);
    if (argc == 3) {
        MSG message;
        HANDLE ready = OpenEventA(EVENT_MODIFY_STATE, FALSE, argv[2]);
        foreign_instance = TRUE;
        frame_a = make_frame(wc.lpszClassName, 40, 320);
        strip_a = make_strip(frame_a);
        CHECK(drag_attach(strip_a));
        if (!ready) return 2;
        SetEvent(ready);
        CloseHandle(ready);
        while (GetMessageW(&message, NULL, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return failures ? 1 : 0;
    }
    frame_a = make_frame(wc.lpszClassName, 40, 60);
    frame_b = make_frame(wc.lpszClassName, 480, 60);
    strip_a = make_strip(frame_a);
    strip_b = make_strip(frame_b);
    b = tab_point(strip_b, 1);
    SetFocus(frame_b);
    mouse(strip_b, WM_LBUTTONDOWN, MK_LBUTTON, b);
    mouse(strip_b, WM_LBUTTONUP, 0, b);
    native_focus = GetFocus() == strip_b;
    CHECK(TabCtrl_GetCurSel(strip_b) == 1);
    CHECK(!drag_attach(NULL));
    CHECK(!drag_attach(frame_a));
    CHECK(drag_attach(strip_a));
    CHECK(drag_attach(strip_a));
    CHECK(drag_attach(strip_b));
    a = tab_point(strip_a, 0);
    b = tab_point(strip_b, 1);
    gesture(a, b);
    CHECK(drops == 1);
    CHECK(original == 0 && received);
    CHECK(plan.target == frame_b && plan.index == 1);
    CHECK(plan.screen.x == b.x && plan.screen.y == b.y);
    CHECK(!drag_take(frame_a, last_token, &unused));
    CHECK(!drag_take(frame_a, (LPARAM)(LONG_PTR)-1, &unused));
    CHECK(TabCtrl_GetItemCount(strip_a) == 3);
    CHECK(TabCtrl_GetItemCount(strip_b) == 3);
    /* Same-strip insertion accounts for removal of the source item. */
    {
        RECT r;
        POINT end;
        int before = drops;
        TabCtrl_GetItemRect(strip_a, 2, &r);
        end.x = r.right - 2; end.y = (r.top + r.bottom) / 2;
        ClientToScreen(strip_a, &end);
        gesture(a, end);
        CHECK(drops == before + 1 && plan.target == frame_a && plan.index == 2);
        CHECK(original == 0);
        CHECK(TabCtrl_GetItemCount(strip_a) == 3);
        gesture(tab_point(strip_a, 2), a);
        CHECK(drops == before + 2 && original == 2 && plan.index == 0);
    }
    /* Ordinary clicks/subthreshold jitter retain native focus/selection. */
    {
        int before = drops;
        POINT click = tab_point(strip_a, 1), jitter = click;
        jitter.x += GetSystemMetrics(SM_CXDRAG) / 2 - 1;
        SetFocus(frame_a);
        gesture(click, jitter);
        CHECK(drops == before);
        CHECK(TabCtrl_GetCurSel(strip_a) == 1);
        CHECK((GetFocus() == strip_a) == native_focus);
        CHECK(GetCapture() != strip_a);
    }
    /* Crossing the threshold and returning is still a drag. */
    {
        int before = drops;
        mouse(strip_a, WM_LBUTTONDOWN, MK_LBUTTON, a);
        mouse(strip_a, WM_MOUSEMOVE, MK_LBUTTON, b);
        mouse(strip_a, WM_LBUTTONUP, 0, a);
        CHECK(drops == before + 1 && plan.target == frame_a);
    }
    /* Escape, capture theft, and cancel mode suppress the pending drop. */
    {
        int before = drops;
        HWND edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE,
            0, 70, 100, 30, frame_a, NULL, GetModuleHandleW(NULL), NULL);
        CHECK(edit != NULL);
        mouse(strip_a, WM_LBUTTONDOWN, MK_LBUTTON, a);
        SetFocus(edit);
        mouse(strip_a, WM_MOUSEMOVE, MK_LBUTTON, b);
        SendMessageW(GetFocus(), WM_KEYDOWN, VK_ESCAPE, 0);
        mouse(strip_a, WM_LBUTTONUP, 0, b);
        CHECK(drops == before && GetCapture() != strip_a);
        /* TCS_FOCUSNEVER leaves Escape at the editor's focused child. */
        {
            LONG_PTR style = GetWindowLongPtrW(strip_a, GWL_STYLE);
            SetWindowLongPtrW(strip_a, GWL_STYLE, style | TCS_FOCUSNEVER);
            SetFocus(edit);
            mouse(strip_a, WM_LBUTTONDOWN, MK_LBUTTON, a);
            CHECK(GetFocus() == edit);
            mouse(strip_a, WM_MOUSEMOVE, MK_LBUTTON, b);
            SendMessageW(edit, WM_KEYDOWN, VK_ESCAPE, 0);
            mouse(strip_a, WM_LBUTTONUP, 0, b);
            CHECK(drops == before && GetCapture() != strip_a);
            SetWindowLongPtrW(strip_a, GWL_STYLE, style);
        }
        mouse(strip_a, WM_LBUTTONDOWN, MK_LBUTTON, a);
        mouse(strip_a, WM_MOUSEMOVE, MK_LBUTTON, b);
        SetCapture(strip_b);
        mouse(strip_a, WM_LBUTTONUP, 0, b);
        CHECK(drops == before);
        ReleaseCapture();
        mouse(strip_a, WM_LBUTTONDOWN, MK_LBUTTON, a);
        mouse(strip_a, WM_MOUSEMOVE, MK_LBUTTON, b);
        SendMessageW(strip_a, WM_CANCELMODE, 0, 0);
        mouse(strip_a, WM_LBUTTONUP, 0, b);
        CHECK(drops == before);
    }
    /* Editor body and disabled/dialog frames are not tear-off destinations. */
    {
        int before = drops;
        POINT body = { 100, 100 };
        LONG_PTR ex = GetWindowLongPtrW(frame_b, GWL_EXSTYLE);
        ClientToScreen(frame_b, &body);
        gesture(a, body);
        CHECK(drops == before);
        EnableWindow(frame_b, FALSE);
        gesture(a, b);
        CHECK(drops == before);
        EnableWindow(frame_b, TRUE);
        SetWindowLongPtrW(frame_b, GWL_EXSTYLE, ex | WS_EX_DLGMODALFRAME);
        gesture(a, b);
        CHECK(drops == before);
        SetWindowLongPtrW(frame_b, GWL_EXSTYLE, ex);
    }
    /* Unattached (controlled) strips and unrelated application windows reject. */
    {
        HWND controlled, other;
        POINT target;
        int before = drops;
        controlled = make_frame(L"NoteChad.Editor", 40, 320);
        other = make_strip(controlled);
        target = tab_point(other, 0);
        gesture(a, target);
        CHECK(drops == before);
        DestroyWindow(controlled);
        wc.lpszClassName = L"Unrelated.DragFixture";
        CHECK(RegisterClassW(&wc) != 0);
        controlled = make_frame(wc.lpszClassName, 40, 320);
        other = make_strip(controlled);
        CHECK(!drag_attach(other));
        target = tab_point(other, 0);
        gesture(a, target);
        CHECK(drops == before);
        DestroyWindow(controlled);
        wc.lpszClassName = L"WorkerW";
        CHECK(RegisterClassW(&wc) != 0);
        controlled = make_frame(wc.lpszClassName, 40, 320);
        other = make_strip(controlled);
        target = tab_point(other, 0);
        gesture(a, target);
        CHECK(drops == before); /* A desktop-like name alone is not the shell. */
        DestroyWindow(controlled);
    }
    /* Empty strip space appends, and no-tab mouse-down never starts a drag. */
    {
        POINT blank = { 350, 10 };
        int before = drops;
        ClientToScreen(strip_b, &blank);
        gesture(a, blank);
        CHECK(drops == before + 1 && plan.target == frame_b && plan.index == 3);
        blank.x = 350; blank.y = 10;
        ClientToScreen(strip_a, &blank);
        gesture(blank, b);
        CHECK(drops == before + 1);
    }
    /* Attachment is legal during construction, before the frame is shown. */
    {
        HWND hidden = make_frame(L"NoteChad.Editor", 40, 320);
        HWND tabs;
        ShowWindow(hidden, SW_HIDE);
        tabs = make_strip(hidden);
        CHECK(drag_attach(tabs));
        DestroyWindow(hidden);
    }
    /* A real second process with matching editor/tab classes is still rejected. */
    {
        WCHAR image[MAX_PATH], command[MAX_PATH + 160];
        WCHAR event_name[96];
        HANDLE ready;
        STARTUPINFOW startup = {0};
        PROCESS_INFORMATION process = {0};
        BOOL created;
        int before = drops;
        startup.cb = sizeof(startup);
        GetModuleFileNameW(NULL, image, ARRAYSIZE(image));
        swprintf_s(event_name, ARRAYSIZE(event_name), L"Local\\NoteChad.DragFixture.%lu", GetCurrentProcessId());
        ready = CreateEventW(NULL, TRUE, FALSE, event_name);
        CHECK(ready != NULL);
        swprintf_s(command, ARRAYSIZE(command), L"\"%s\" --foreign %s", image, event_name);
        created = CreateProcessW(image, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process);
        CHECK(created);
        if (created) {
            FindProcessWindow find = { process.dwProcessId, NULL };
            DWORD exit_code = 99;
            CHECK(WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0);
            EnumWindows(find_process_window, (LPARAM)&find);
            CHECK(find.window != NULL);
            if (find.window) {
                HWND tabs = FindWindowExW(find.window, NULL, WC_TABCONTROLW, NULL);
                POINT target = { 20, 10 };
                CHECK(tabs != NULL && !drag_attach(tabs));
                ClientToScreen(tabs, &target);
                gesture(a, target);
                CHECK(drops == before);
                PostMessageW(find.window, WM_CLOSE, 0, 0);
            }
            if (WaitForSingleObject(process.hProcess, 3000) != WAIT_OBJECT_0) {
                CHECK(FALSE);
                TerminateProcess(process.hProcess, 3);
                WaitForSingleObject(process.hProcess, 3000);
            }
            GetExitCodeProcess(process.hProcess, &exit_code);
            CHECK(exit_code == 0);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
        if (ready) CloseHandle(ready);
    }
    /* Find exposed desktop without moving the physical cursor. */
    {
        POINT desktop = {0};
        int x, y;
        BOOL found = FALSE;
        int before = drops;
        for (y = 0; y < GetSystemMetrics(SM_CYSCREEN) && !found; y += 20) {
            for (x = 0; x < GetSystemMetrics(SM_CXSCREEN); x += 20) {
                WCHAR name[64] = {0};
                HWND hit;
                desktop.x = x; desktop.y = y;
                hit = WindowFromPoint(desktop);
                GetClassNameW(GetAncestor(hit, GA_ROOT), name, ARRAYSIZE(name));
                if (!hit || !lstrcmpW(name, L"Progman") || !lstrcmpW(name, L"WorkerW")) {
                    found = TRUE;
                    break;
                }
            }
        }
        if (!found) {
            /* A maximized user window may cover every desktop pixel. Exercise
               the genuine empty-area path without minimizing anything. */
            desktop.x = GetSystemMetrics(SM_XVIRTUALSCREEN) - 40;
            desktop.y = GetSystemMetrics(SM_YVIRTUALSCREEN) - 40;
            found = WindowFromPoint(desktop) == NULL;
        }
        CHECK(found);
        if (found) {
            gesture(a, desktop);
            CHECK(drops == before + 1 && plan.target == NULL && plan.index == 0);
            CHECK(plan.screen.x == desktop.x && plan.screen.y == desktop.y);
        }
    }
    DestroyWindow(frame_b);
    DestroyWindow(frame_a);
    printf("drag: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
