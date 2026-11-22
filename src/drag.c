#include "drag.h"
#include <windowsx.h>
#include <commctrl.h>
#include <wchar.h>

#ifndef WM_DPICHANGED_AFTERPARENT
#define WM_DPICHANGED_AFTERPARENT 0x02E3
#endif

#define DRAG_SUBCLASS ((UINT_PTR)0x4E434447)
#define DRAG_PROPERTY L"NoteChad.TabDrag.Attached"

typedef struct {
    BOOL tracking, dragging;
    int index;
    POINT start;
    HWND keyboard;
    int closing, hot; /* 1=left close, 2=middle close; never a drag */
    LPARAM identity;
    RECT close_rect;
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
    if (process != GetCurrentProcessId() || !named(frame, L"NoteChad.Editor"))
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

/* Keep the text and accessibility implementation native. Symmetric native
   padding reserves enough trailing space for an independent close surface. */
static int scaled(HWND strip, int value)
{
    typedef UINT (WINAPI *DpiFn)(HWND);
    DpiFn dpi_fn = (DpiFn)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    UINT dpi = dpi_fn ? dpi_fn(strip) : 0;
    if (!dpi) {
        HDC dc = GetDC(strip);
        dpi = dc ? (UINT)GetDeviceCaps(dc, LOGPIXELSX) : 96;
        if (dc) ReleaseDC(strip, dc);
    }
    return MulDiv(value, (int)dpi, 96);
}

static void close_padding(HWND strip)
{
    TabCtrl_SetPadding(strip, scaled(strip, 24), scaled(strip, 4));
}

BOOL drag_close_rect(HWND strip, int index, RECT *out)
{
    RECT tab, client, r;
    HWND child;
    int size = scaled(strip, 18), gap = scaled(strip, 3);
    if (!out || index < 0 || !GetPropW(strip, DRAG_PROPERTY) ||
        !TabCtrl_GetItemRect(strip, index, &tab)) return FALSE;
    r.right = tab.right - gap;
    r.left = r.right - size;
    r.top = (tab.top + tab.bottom - size) / 2;
    r.bottom = r.top + size;
    GetClientRect(strip, &client);
    if (r.left < tab.left || r.top < tab.top || r.bottom > tab.bottom ||
        r.left < client.left || r.right > client.right ||
        r.top < client.top || r.bottom > client.bottom) return FALSE;
    /* The native overflow child can cover part of an otherwise valid tab. */
    for (child = GetWindow(strip, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        RECT cover, overlap;
        if (!IsWindowVisible(child)) continue;
        GetWindowRect(child, &cover);
        MapWindowPoints(NULL, strip, (POINT *)&cover, 2);
        if (IntersectRect(&overlap, &r, &cover)) return FALSE;
    }
    /* Selected native tabs can overlap their neighbours by a few pixels. */
    {
        TCHITTESTINFO hit = {0};
        hit.pt.x = r.left; hit.pt.y = (r.top + r.bottom) / 2;
        if (TabCtrl_HitTest(strip, &hit) != index) return FALSE;
        hit.pt.x = r.right - 1;
        if (TabCtrl_HitTest(strip, &hit) != index) return FALSE;
    }
    *out = r;
    return TRUE;
}

static int close_hit(HWND strip, POINT point)
{
    TCHITTESTINFO hit = {0};
    RECT r;
    int index;
    hit.pt = point;
    index = TabCtrl_HitTest(strip, &hit);
    return drag_close_rect(strip, index, &r) && PtInRect(&r, point) ? index : -1;
}

static BOOL tab_identity(HWND strip, int index, LPARAM *identity)
{
    TCITEMW item = {0};
    item.mask = TCIF_PARAM;
    if (!TabCtrl_GetItem(strip, index, &item)) return FALSE;
    *identity = item.lParam; /* Opaque identity only; never dereferenced. */
    return TRUE;
}

static void paint_close(HWND strip, HDC dc, DragState *state)
{
    int i, count = TabCtrl_GetItemCount(strip);
    int save = SaveDC(dc);
    HWND child;
    for (child = GetWindow(strip, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        RECT cover;
        if (!IsWindowVisible(child)) continue;
        GetWindowRect(child, &cover);
        MapWindowPoints(NULL, strip, (POINT *)&cover, 2);
        ExcludeClipRect(dc, cover.left, cover.top, cover.right, cover.bottom);
    }
    for (i = 0; i < count; ++i) {
        RECT r;
        HPEN pen, old;
        int inset = scaled(strip, 5);
        BOOL enabled = IsWindowEnabled(strip) && IsWindowEnabled(GetParent(strip));
        COLORREF color = GetSysColor(enabled ? COLOR_BTNTEXT : COLOR_GRAYTEXT);
        if (!drag_close_rect(strip, i, &r)) continue;
        if (enabled && (state->hot == i || (state->closing && state->index == i))) {
            FillRect(dc, &r, GetSysColorBrush(state->closing && state->index == i ? COLOR_3DSHADOW : COLOR_HIGHLIGHT));
            color = GetSysColor(COLOR_HIGHLIGHTTEXT);
        }
        pen = CreatePen(PS_SOLID, scaled(strip, 1), color);
        if (!pen) continue;
        old = SelectObject(dc, pen);
        MoveToEx(dc, r.left + inset, r.top + inset, NULL);
        LineTo(dc, r.right - inset, r.bottom - inset);
        MoveToEx(dc, r.right - inset - 1, r.top + inset, NULL);
        LineTo(dc, r.left + inset - 1, r.bottom - inset);
        SelectObject(dc, old);
        DeleteObject(pen);
    }
    RestoreDC(dc, save);
}

static LRESULT CALLBACK strip_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
static LRESULT CALLBACK keyboard_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

static void stop(HWND strip, DragState *state)
{
    HWND keyboard = state->keyboard;
    state->keyboard = NULL;
    state->tracking = state->dragging = FALSE;
    state->closing = 0;
    state->hot = -1;
    InvalidateRect(strip, NULL, FALSE);
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
    case WM_PAINT:
    case WM_PRINTCLIENT: {
        LRESULT result = DefSubclassProc(strip, message, wparam, lparam);
        DWORD_PTR current;
        HDC dc;
        if (!IsWindow(strip) || !GetWindowSubclass(strip, strip_proc, DRAG_SUBCLASS, &current) ||
            current != reference) return result;
        dc = message == WM_PRINTCLIENT ? (HDC)wparam : GetDC(strip);
        if (dc) paint_close(strip, dc, state);
        if (dc && message == WM_PAINT) ReleaseDC(strip, dc);
        return result;
    }
    case WM_LBUTTONDBLCLK:
        if (close_hit(strip, point) < 0) break;
        /* Native controls deliver DBLCLK in place of the second DOWN. */
        message = WM_LBUTTONDOWN;
        /* fall through */
    case WM_MBUTTONDOWN:
    case WM_LBUTTONDOWN: {
        TCHITTESTINFO hit = {0};
        LRESULT result;
        DWORD_PTR current = 0;
        int index;
        hit.pt = point;
        index = TabCtrl_HitTest(strip, &hit);
        stop(strip, state);
        if (index >= 0 && (message == WM_MBUTTONDOWN || close_hit(strip, point) == index) &&
            ordinary(GetParent(strip)) && IsWindowEnabled(strip)) {
            RECT r;
            if (!drag_close_rect(strip, index, &r) || !tab_identity(strip, index, &state->identity)) return 0;
            state->closing = message == WM_MBUTTONDOWN ? 2 : 1;
            state->index = index;
            state->close_rect = r;
            SetCapture(strip);
            if (GetCapture() != strip) { stop(strip, state); return 0; }
            {
                HWND focus = GetFocus();
                if (focus && focus != strip && (focus == GetParent(strip) || IsChild(GetParent(strip), focus))) {
                    if (SetWindowSubclass(focus, keyboard_proc, (UINT_PTR)strip, (DWORD_PTR)strip))
                        state->keyboard = focus;
                    else stop(strip, state);
                }
            }
            InvalidateRect(strip, &r, FALSE);
            return 0;
        }
        if (message == WM_MBUTTONDOWN) return 0;
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
    case WM_MOUSELEAVE:
        if (state->closing) stop(strip, state);
        if (state->hot != -1) { state->hot = -1; InvalidateRect(strip, NULL, FALSE); }
        break;
    case WM_MOUSEMOVE:
        if (state->closing) {
            RECT r;
            LPARAM identity;
            TCHITTESTINFO hit = {0};
            hit.pt = point;
            if (!drag_close_rect(strip, state->index, &r) || !EqualRect(&r, &state->close_rect) ||
                !tab_identity(strip, state->index, &identity) || identity != state->identity ||
                (state->closing == 1 ? close_hit(strip, point) : TabCtrl_HitTest(strip, &hit)) != state->index)
                stop(strip, state);
            return 0;
        }
        if (!state->tracking) {
            int hot = close_hit(strip, point);
            TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, strip, 0 };
            TrackMouseEvent(&track);
            if (hot != state->hot) { state->hot = hot; InvalidateRect(strip, NULL, FALSE); }
        }
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
    case WM_MBUTTONUP:
    case WM_LBUTTONUP:
        if (state->closing) {
            int index = state->index;
            RECT r;
            LPARAM identity;
            TCHITTESTINFO hit = {0};
            HWND parent = GetParent(strip);
            BOOL valid;
            hit.pt = point;
            valid = (message == WM_MBUTTONUP ? state->closing == 2 : state->closing == 1) &&
                GetCapture() == strip && ordinary(parent) && IsWindowEnabled(strip) &&
                drag_close_rect(strip, index, &r) && EqualRect(&r, &state->close_rect) &&
                tab_identity(strip, index, &identity) && identity == state->identity &&
                (state->closing == 1 ? close_hit(strip, point) : TabCtrl_HitTest(strip, &hit)) == index;
            stop(strip, state);
            /* Parent may synchronously destroy the strip/state. No access after send. */
            if (valid) return SendMessageW(parent, WM_NC_TABCLOSE, (WPARAM)index, (LPARAM)strip);
            return 0;
        }
        if (message == WM_MBUTTONUP) return 0;
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
        if (wparam == VK_ESCAPE && (state->tracking || state->closing)) {
            stop(strip, state);
            return 0;
        }
        break;
    case WM_SETFONT:
    case WM_DPICHANGED_AFTERPARENT: {
        LRESULT result;
        DWORD_PTR current;
        stop(strip, state);
        result = DefSubclassProc(strip, message, wparam, lparam);
        if (IsWindow(strip) && GetWindowSubclass(strip, strip_proc, DRAG_SUBCLASS, &current) && current == reference) {
            close_padding(strip);
            InvalidateRect(strip, NULL, TRUE);
        }
        return result;
    }
    case TCM_INSERTITEMA:
    case TCM_INSERTITEMW:
    case TCM_DELETEITEM:
    case TCM_DELETEALLITEMS:
    case TCM_SETITEMA:
    case TCM_SETITEMW:
    case TCM_SETCURSEL:
    case TCM_SETITEMSIZE:
    case TCM_SETPADDING:
    case WM_SIZE:
    case WM_STYLECHANGED:
    case WM_HSCROLL:
    case WM_VSCROLL:
    case WM_ENABLE:
    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE:
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
    state->hot = -1;
    close_padding(strip);
    InvalidateRect(strip, NULL, TRUE);
    return TRUE;
}
