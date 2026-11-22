#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <stdio.h>
#include "drag.h"

static int checks, failures, closes, closed_index = -1, drops;
static BOOL destroy_on_close;
static HWND closed_strip;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)
static LRESULT CALLBACK frame_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_NC_TABCLOSE) {
        ++closes; closed_index = (int)w; closed_strip = (HWND)l;
        if (destroy_on_close) DestroyWindow((HWND)l);
        return 0;
    }
    if (m == WM_NC_TABDROP) { ++drops; return 0; }
    return DefWindowProcW(h, m, w, l);
}
static void mouse(HWND h, UINT m, WPARAM keys, POINT p)
{
    SendMessageW(h, m, keys, MAKELPARAM(p.x, p.y));
}
static POINT close_point(HWND h, int i)
{
    RECT r;
    POINT p = {0};
    CHECK(TabCtrl_GetItemRect(h, i, &r));
    CHECK(drag_close_rect(h, i, &r));
    p.x = (r.left + r.right) / 2; p.y = (r.top + r.bottom) / 2;
    return p;
}
static COLORREF printed_pixel(HWND strip, POINT p)
{
    RECT r;
    HDC screen = GetDC(strip), dc = CreateCompatibleDC(screen);
    HBITMAP bitmap, old;
    COLORREF pixel;
    GetClientRect(strip, &r);
    bitmap = CreateCompatibleBitmap(screen, r.right, r.bottom);
    old = SelectObject(dc, bitmap);
    FillRect(dc, &r, GetSysColorBrush(COLOR_WINDOW));
    SendMessageW(strip, WM_PRINTCLIENT, (WPARAM)dc, PRF_CLIENT | PRF_ERASEBKGND);
    pixel = GetPixel(dc, p.x, p.y);
    SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc); ReleaseDC(strip, screen);
    return pixel;
}
int main(void)
{
    INITCOMMONCONTROLSEX ic = {sizeof(ic), ICC_TAB_CLASSES};
    WNDCLASSW wc = {0};
    HWND frame, strip;
    int i;
    POINT p;
    InitCommonControlsEx(&ic);
    wc.lpfnWndProc = frame_proc; wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"NoteChad.Editor";
    CHECK(RegisterClassW(&wc));
    frame = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Close fixture",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 60, 600, 200, NULL, NULL, wc.hInstance, NULL);
    CHECK(frame != NULL);
    strip = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE,
        0, 0, 570, 50, frame, NULL, wc.hInstance, NULL);
    CHECK(strip != NULL);
    for (i = 0; i < 3; ++i) {
        TCITEMW item = {0};
        item.mask = TCIF_TEXT | TCIF_PARAM; item.pszText = L"Accessible label"; item.lParam = i + 1;
        CHECK(TabCtrl_InsertItem(strip, i, &item) == i);
    }
    CHECK(drag_attach(strip));
    p = close_point(strip, 1);
    mouse(strip, WM_LBUTTONDOWN, MK_LBUTTON, p);
    CHECK(GetCapture() == strip);
    mouse(strip, WM_LBUTTONUP, 0, p);
    CHECK(closes == 1 && closed_index == 1 && closed_strip == strip);
    CHECK(TabCtrl_GetCurSel(strip) == 0); /* Close does not activate an inactive tab. */
    CHECK(drops == 0 && GetCapture() != strip);
    /* Actual offscreen native printing contains the X and distinct affordances. */
    {
        RECT r;
        POINT glyph, corner;
        int inset;
        CHECK(drag_close_rect(strip, 1, &r));
        inset = MulDiv(5, r.right - r.left, 18);
        glyph.x = r.left + inset; glyph.y = r.top + inset;
        corner.x = r.left + 1; corner.y = r.top + 1;
        CHECK(printed_pixel(strip, glyph) == GetSysColor(COLOR_BTNTEXT));
        mouse(strip, WM_MOUSEMOVE, 0, p);
        CHECK(printed_pixel(strip, corner) == GetSysColor(COLOR_HIGHLIGHT));
        CHECK(printed_pixel(strip, glyph) == GetSysColor(COLOR_HIGHLIGHTTEXT));
        mouse(strip, WM_LBUTTONDOWN, MK_LBUTTON, p);
        CHECK(printed_pixel(strip, corner) == GetSysColor(COLOR_3DSHADOW));
        SendMessageW(strip, WM_CANCELMODE, 0, 0);
        EnableWindow(strip, FALSE);
        CHECK(printed_pixel(strip, glyph) == GetSysColor(COLOR_GRAYTEXT));
        EnableWindow(strip, TRUE);
        InvalidateRect(strip, NULL, TRUE); UpdateWindow(strip);
        {
            HDC dc = GetDC(strip);
            CHECK(GetPixel(dc, glyph.x, glyph.y) == GetSysColor(COLOR_BTNTEXT));
            ReleaseDC(strip, dc);
        }
    }
    /* A native double-click's second down must remain a close surface, not select. */
    {
        int before = closes;
        mouse(strip, WM_LBUTTONDBLCLK, MK_LBUTTON, p);
        mouse(strip, WM_LBUTTONUP, 0, p);
        CHECK(closes == before + 1 && TabCtrl_GetCurSel(strip) == 0);
    }
    /* Full surface: corners hit the same tab and release requests only it. */
    {
        RECT r;
        POINT edges[4];
        CHECK(drag_close_rect(strip, 1, &r));
        edges[0].x = r.left; edges[0].y = r.top;
        edges[1].x = r.right - 1; edges[1].y = r.top;
        edges[2].x = r.left; edges[2].y = r.bottom - 1;
        edges[3].x = r.right - 1; edges[3].y = r.bottom - 1;
        for (i = 0; i < 4; ++i) {
            int before = closes;
            TCHITTESTINFO hit = {0}; hit.pt = edges[i];
            CHECK(TabCtrl_HitTest(strip, &hit) == 1);
            mouse(strip, WM_LBUTTONDOWN, MK_LBUTTON, edges[i]);
            mouse(strip, WM_LBUTTONUP, 0, edges[i]);
            CHECK(closes == before + 1 && closed_index == 1);
        }
    }
    /* Moving away permanently cancels, even if the pointer comes back. */
    {
        int before = closes;
        POINT other = close_point(strip, 2);
        mouse(strip, WM_LBUTTONDOWN, MK_LBUTTON, p);
        mouse(strip, WM_MOUSEMOVE, MK_LBUTTON, other);
        mouse(strip, WM_MOUSEMOVE, MK_LBUTTON, p);
        mouse(strip, WM_LBUTTONUP, 0, p);
        CHECK(closes == before && drops == 0 && GetCapture() != strip);
        mouse(strip, WM_LBUTTONDOWN, MK_LBUTTON, p);
        mouse(strip, WM_LBUTTONUP, 0, other);
        CHECK(closes == before);
    }
    /* Cancellation at the strip and at the existing editor keyboard owner. */
    {
        int before = closes;
        HWND edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE,
            0, 60, 100, 20, frame, NULL, wc.hInstance, NULL);
        CHECK(edit != NULL);
        SetFocus(edit);
        mouse(strip, WM_LBUTTONDOWN, MK_LBUTTON, p);
        CHECK(GetFocus() == edit);
        SendMessageW(edit, WM_KEYDOWN, VK_ESCAPE, 0);
        mouse(strip, WM_LBUTTONUP, 0, p);
        CHECK(closes == before && GetCapture() != strip);
        mouse(strip, WM_LBUTTONDOWN, MK_LBUTTON, p);
        SendMessageW(strip, WM_KEYDOWN, VK_ESCAPE, 0);
        mouse(strip, WM_LBUTTONUP, 0, p);
        CHECK(closes == before);
        mouse(strip, WM_LBUTTONDOWN, MK_LBUTTON, p);
        SetCapture(edit);
        mouse(strip, WM_LBUTTONUP, 0, p);
        CHECK(closes == before);
        ReleaseCapture();
        mouse(strip, WM_LBUTTONDOWN, MK_LBUTTON, p);
        SendMessageW(strip, WM_CANCELMODE, 0, 0);
        mouse(strip, WM_LBUTTONUP, 0, p);
        CHECK(closes == before);
        DestroyWindow(edit);
    }
    /* Same index and label with a new identity must not close the replacement. */
    {
        int before = closes;
        TCITEMW item = {0}; item.mask = TCIF_PARAM; item.lParam = 99;
        mouse(strip, WM_LBUTTONDOWN, MK_LBUTTON, p);
        CHECK(TabCtrl_SetItem(strip, 1, &item));
        mouse(strip, WM_LBUTTONUP, 0, p);
        CHECK(closes == before && GetCapture() != strip);
        item.mask = TCIF_TEXT | TCIF_PARAM; item.pszText = L"Accessible label";
        mouse(strip, WM_LBUTTONDOWN, MK_LBUTTON, p);
        CHECK(TabCtrl_DeleteItem(strip, 1));
        CHECK(TabCtrl_InsertItem(strip, 1, &item) == 1);
        mouse(strip, WM_LBUTTONUP, 0, p);
        CHECK(closes == before);
    }
    /* Middle closes from a label without selecting it; left label stays native. */
    {
        RECT r;
        POINT label;
        int before = closes;
        CHECK(TabCtrl_GetItemRect(strip, 2, &r));
        label.x = (r.left + r.right) / 2; label.y = (r.top + r.bottom) / 2;
        mouse(strip, WM_MBUTTONDOWN, MK_MBUTTON, label);
        mouse(strip, WM_MBUTTONUP, 0, label);
        CHECK(closes == before + 1 && closed_index == 2 && TabCtrl_GetCurSel(strip) == 0);
        mouse(strip, WM_LBUTTONDOWN, MK_LBUTTON, label);
        mouse(strip, WM_LBUTTONUP, 0, label);
        CHECK(closes == before + 1 && TabCtrl_GetCurSel(strip) == 2 && drops == 0);
        SetFocus(strip);
        SendMessageW(strip, WM_KEYDOWN, VK_LEFT, 0);
        CHECK(TabCtrl_GetCurSel(strip) == 1 && closes == before + 1);
    }
    /* Unicode labels are intact; native measured text has reserved X space. */
    {
        WCHAR text[64];
        TCITEMW item = {0};
        RECT tab, close;
        HDC dc = GetDC(strip);
        HFONT font = (HFONT)SendMessageW(strip, WM_GETFONT, 0, 0), old = NULL;
        SIZE size;
        item.mask = TCIF_TEXT; item.pszText = L"日本語 😀 long";
        CHECK(TabCtrl_SetItem(strip, 0, &item));
        item.pszText = text; item.cchTextMax = ARRAYSIZE(text);
        CHECK(TabCtrl_GetItem(strip, 0, &item));
        CHECK(!lstrcmpW(text, L"日本語 😀 long"));
        CHECK(TabCtrl_GetItemRect(strip, 0, &tab));
        CHECK(drag_close_rect(strip, 0, &close));
        if (font) old = SelectObject(dc, font);
        GetTextExtentPoint32W(dc, text, lstrlenW(text), &size);
        CHECK((tab.right - tab.left - size.cx) / 2 >= tab.right - close.left);
        if (old) SelectObject(dc, old);
        ReleaseDC(strip, dc);
    }
    /* Font/DPI relayout, disable, and overflow arrow never retarget a press. */
    {
        int before = closes;
        HWND arrow;
        RECT r;
        p = close_point(strip, 1);
        mouse(strip, WM_LBUTTONDOWN, MK_LBUTTON, p);
        SendMessageW(strip, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        mouse(strip, WM_LBUTTONUP, 0, p);
        CHECK(closes == before);
        p = close_point(strip, 1);
        mouse(strip, WM_LBUTTONDOWN, MK_LBUTTON, p);
        SendMessageW(strip, 0x02E3, 0, 0);
        mouse(strip, WM_LBUTTONUP, 0, p);
        CHECK(closes == before);
        EnableWindow(strip, FALSE);
        mouse(strip, WM_LBUTTONDOWN, MK_LBUTTON, p);
        mouse(strip, WM_LBUTTONUP, 0, p);
        CHECK(closes == before);
        EnableWindow(strip, TRUE);
        p = close_point(strip, 1);
        mouse(strip, WM_LBUTTONDOWN, MK_LBUTTON, p);
        SetWindowPos(strip, NULL, 0, 0, 180, 50, SWP_NOMOVE | SWP_NOZORDER);
        mouse(strip, WM_LBUTTONUP, 0, p);
        CHECK(closes == before);
        UpdateWindow(strip);
        arrow = FindWindowExW(strip, NULL, UPDOWN_CLASSW, NULL);
        CHECK(arrow != NULL && IsWindowVisible(arrow));
        if (arrow) {
            POINT over;
            GetWindowRect(arrow, &r); MapWindowPoints(NULL, strip, (POINT *)&r, 2);
            over.x = (r.left + r.right) / 2; over.y = (r.top + r.bottom) / 2;
            mouse(strip, WM_LBUTTONDOWN, MK_LBUTTON, over);
            mouse(strip, WM_LBUTTONUP, 0, over);
            CHECK(closes == before);
            for (i = 0; i < TabCtrl_GetItemCount(strip); ++i) {
                RECT close, overlap;
                if (drag_close_rect(strip, i, &close)) CHECK(!IntersectRect(&overlap, &r, &close));
            }
        }
        CHECK(!drag_close_rect(strip, 2, &r));
        SetWindowPos(strip, NULL, 0, 0, 250, 50, SWP_NOMOVE | SWP_NOZORDER);
        TabCtrl_SetCurSel(strip, 0);
        p = close_point(strip, 0);
        if (arrow) {
            POINT over;
            GetWindowRect(arrow, &r); MapWindowPoints(NULL, strip, (POINT *)&r, 2);
            over.x = (r.left + r.right) / 2; over.y = (r.top + r.bottom) / 2;
            mouse(strip, WM_LBUTTONDOWN, MK_LBUTTON, p);
            mouse(strip, WM_MOUSEMOVE, MK_LBUTTON, over);
            mouse(strip, WM_LBUTTONUP, 0, p);
            CHECK(closes == before && GetCapture() != strip);
        }
        SetWindowPos(strip, NULL, 0, 0, 570, 50, SWP_NOMOVE | SWP_NOZORDER);
    }
    /* Parent can destroy the exact target synchronously in its close handler. */
    p = close_point(strip, 0);
    destroy_on_close = TRUE;
    i = closes;
    mouse(strip, WM_LBUTTONDOWN, MK_LBUTTON, p);
    mouse(strip, WM_LBUTTONUP, 0, p);
    CHECK(closes == i + 1 && !IsWindow(strip));
    DestroyWindow(frame);
    printf("tabclose: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
