#include "drag.h"
#include <windowsx.h>
#include <commctrl.h>
#include <wchar.h>

#define DRAG_SUBCLASS ((UINT_PTR)0x4E434447)
#define DRAG_PROPERTY L"NodeChad.TabDrag.Attached"

typedef struct {
    BOOL tracking, dragging;
    int index;
    POINT start;
    HWND keyboard;
} DragState;

typedef struct Pending {
    struct Pending *previous;
    HWND source;
    LPARAM token;
    BOOL taken;
    TabDrop plan;
} Pending;

static __declspec(thread) Pending *pending;
static volatile LONG64 next_token;

static BOOL named(HWND window, const WCHAR *name)
{
    WCHAR text[64];
    return GetClassNameW(window, text, ARRAYSIZE(text)) && !wcscmp(text, name);
}

static BOOL editor_frame(HWND frame)
{
    DWORD process = 0;
    LONG_PTR style, ex;
    GetWindowThreadProcessId(frame, &process);
    if (process != GetCurrentProcessId() || !named(frame, L"NodeChad.Editor"))
        return FALSE;
    style = GetWindowLongPtrW(frame, GWL_STYLE);
    ex = GetWindowLongPtrW(frame, GWL_EXSTYLE);
    return !(style & (WS_CHILD | WS_POPUP)) && (style & WS_CAPTION) == WS_CAPTION &&
        !(ex & WS_EX_DLGMODALFRAME) && !GetWindow(frame, GW_OWNER);
}

static BOOL ordinary(HWND frame)
{
    return editor_frame(frame) && IsWindowVisible(frame) && IsWindowEnabled(frame);
}

BOOL drag_take(HWND source, LPARAM token, TabDrop *out)
{
    Pending *p;
    if (!out || !token) return FALSE;
    /* The caller's token is only compared, never converted into a pointer. */
    for (p = pending; p; p = p->previous) {
        if (p->token == token && p->source == source && !p->taken) {
            *out = p->plan;
            p->taken = TRUE;
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL destination(HWND source_strip, int source_index, POINT screen, TabDrop *out)
{
    HWND hit = WindowFromPoint(screen), root, strip;
    POINT local = screen;
    RECT r;
    int count, i, index;
    out->screen = screen;
    out->target = NULL;
    out->index = 0;
    if (!hit) return TRUE;
    root = GetAncestor(hit, GA_ROOT);
    /* Shell desktop is the sole exception to the current-process rule.
       No application data or pointer is sent to it. */
    if (named(root, L"Progman") || named(root, L"WorkerW")) {
        DWORD shell_process = 0, hit_process = 0;
        GetWindowThreadProcessId(GetShellWindow(), &shell_process);
        GetWindowThreadProcessId(root, &hit_process);
        return shell_process != 0 && shell_process == hit_process;
    }
    if (!ordinary(root)) return FALSE;
    strip = hit;
    if (hit == root) {
        /* Native tabs return HTTRANSPARENT over trailing strip space. */
        for (strip = GetWindow(root, GW_CHILD); strip; strip = GetWindow(strip, GW_HWNDNEXT)) {
            if (named(strip, WC_TABCONTROLW) && IsWindowVisible(strip) &&
                GetWindowRect(strip, &r) && PtInRect(&r, screen)) break;
        }
    } else {
        while (strip && GetParent(strip) != root) strip = GetParent(strip);
    }
    if (!strip || !named(strip, WC_TABCONTROLW) ||
        !GetPropW(strip, DRAG_PROPERTY) || !IsWindowEnabled(strip)) return FALSE;
    ScreenToClient(strip, &local);
    GetClientRect(strip, &r);
    if (!PtInRect(&r, local)) return FALSE;
    count = TabCtrl_GetItemCount(strip);
    index = count;
    for (i = 0; i < count; ++i) {
        if (TabCtrl_GetItemRect(strip, i, &r) &&
            (local.y < r.top || (local.y < r.bottom && local.x <= (r.left + r.right) / 2))) {
            index = i;
            break;
        }
    }
    if (strip == source_strip && index > source_index) --index;
    out->target = root;
    out->index = index;
    return TRUE;
}

static LRESULT CALLBACK strip_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
static LRESULT CALLBACK keyboard_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

static void stop(HWND strip, DragState *state)
{
    HWND keyboard = state->keyboard;
    state->keyboard = NULL;
    state->tracking = state->dragging = FALSE;
    if (keyboard) RemoveWindowSubclass(keyboard, keyboard_proc, (UINT_PTR)strip);
    if (GetCapture() == strip) ReleaseCapture();
}

/* Capture routes mouse messages, not keys. Observe only this gesture's native
   focus owner, without changing focus or installing any hook. Focus changes
   cancel the gesture rather than leaving an unobservable Escape path. */
static LRESULT CALLBACK keyboard_proc(HWND window, UINT message, WPARAM wparam,
                                      LPARAM lparam, UINT_PTR id, DWORD_PTR reference)
{
    HWND strip = (HWND)reference;
    DWORD_PTR state;
    BOOL escape = (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && wparam == VK_ESCAPE;
    (void)id;
    if ((escape || message == WM_KILLFOCUS || message == WM_NCDESTROY) &&
        GetWindowSubclass(strip, strip_proc, DRAG_SUBCLASS, &state)) {
        stop(strip, (DragState *)state);
        if (escape) return 0;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

static LRESULT CALLBACK strip_proc(HWND strip, UINT message, WPARAM wparam,
                                   LPARAM lparam, UINT_PTR id, DWORD_PTR reference)
{
    DragState *state = (DragState *)reference;
    POINT point = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
    (void)id;
    switch (message) {
    case WM_LBUTTONDOWN: {
        TCHITTESTINFO hit = {0};
        LRESULT result;
        DWORD_PTR current = 0;
        int index;
        hit.pt = point;
        index = TabCtrl_HitTest(strip, &hit);
        stop(strip, state);
        /* Native control owns ordinary focus, selection and parent notifications. */
        result = DefSubclassProc(strip, message, wparam, lparam);
        if (!IsWindow(strip) || !GetWindowSubclass(strip, strip_proc, DRAG_SUBCLASS, &current) ||
            current != reference) return result;
        if (index >= 0 && ordinary(GetParent(strip)) && IsWindowEnabled(strip)) {
            state->tracking = TRUE;
            state->index = index;
            ClientToScreen(strip, &point);
            state->start = point;
            SetCapture(strip);
            if (GetCapture() != strip) state->tracking = FALSE;
            if (state->tracking) {
                HWND focus = GetFocus();
                if (focus && focus != strip && (focus == GetParent(strip) || IsChild(GetParent(strip), focus))) {
                    if (SetWindowSubclass(focus, keyboard_proc, (UINT_PTR)strip, (DWORD_PTR)strip))
                        state->keyboard = focus;
                    else stop(strip, state);
                }
            }
        }
        return result;
    }
    case WM_MOUSEMOVE:
        if (state->tracking && (wparam & MK_LBUTTON)) {
            RECT threshold;
            int cx = GetSystemMetrics(SM_CXDRAG), cy = GetSystemMetrics(SM_CYDRAG);
            ClientToScreen(strip, &point);
            threshold.left = state->start.x - cx / 2;
            threshold.top = state->start.y - cy / 2;
            threshold.right = threshold.left + cx;
            threshold.bottom = threshold.top + cy;
            if (!PtInRect(&threshold, point)) state->dragging = TRUE;
        }
        break;
    case WM_LBUTTONUP:
        if (state->tracking) {
            BOOL dragging = state->dragging;
            int index = state->index;
            HWND source = GetParent(strip);
            TabDrop plan = {0};
            BOOL valid;
            LRESULT result;
            ClientToScreen(strip, &point);
            valid = dragging && ordinary(source) && destination(strip, index, point, &plan);
            stop(strip, state);
            result = DefSubclassProc(strip, message, wparam, lparam);
            if (valid && IsWindow(strip) && ordinary(source)) {
                Pending p;
                p.previous = pending;
                p.source = source;
                p.token = (LPARAM)InterlockedIncrement64(&next_token);
                p.taken = FALSE;
                p.plan = plan;
                pending = &p;
                SendMessageW(source, WM_NC_TABDROP, (WPARAM)index, p.token);
                pending = p.previous;
            }
            return result;
        }
        break;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE && state->tracking) {
            stop(strip, state);
            return 0;
        }
        break;
    case WM_CANCELMODE:
        stop(strip, state);
        break;
    case WM_KILLFOCUS:
    case WM_CAPTURECHANGED:
        stop(strip, state);
        break;
    case WM_NCDESTROY:
        stop(strip, state);
        RemovePropW(strip, DRAG_PROPERTY);
        RemoveWindowSubclass(strip, strip_proc, DRAG_SUBCLASS);
        HeapFree(GetProcessHeap(), 0, state);
        break;
    }
    return DefSubclassProc(strip, message, wparam, lparam);
}

BOOL drag_attach(HWND strip)
{
    DWORD process = 0;
    DWORD thread = GetWindowThreadProcessId(strip, &process);
    DWORD_PTR existing;
    DragState *state;
    if (process != GetCurrentProcessId() || thread != GetCurrentThreadId() ||
        !named(strip, WC_TABCONTROLW) || !(GetWindowLongPtrW(strip, GWL_STYLE) & WS_CHILD) ||
        !editor_frame(GetParent(strip))) return FALSE;
    if (GetWindowSubclass(strip, strip_proc, DRAG_SUBCLASS, &existing)) return TRUE;
    state = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*state));
    if (!state) return FALSE;
    if (!SetPropW(strip, DRAG_PROPERTY, (HANDLE)(UINT_PTR)1)) {
        HeapFree(GetProcessHeap(), 0, state);
        return FALSE;
    }
    if (!SetWindowSubclass(strip, strip_proc, DRAG_SUBCLASS, (DWORD_PTR)state)) {
        RemovePropW(strip, DRAG_PROPERTY);
        HeapFree(GetProcessHeap(), 0, state);
        return FALSE;
    }
    return TRUE;
}
