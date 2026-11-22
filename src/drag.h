#ifndef NOTECHAD_DRAG_H
#define NOTECHAD_DRAG_H
#include <windows.h>

#define WM_NC_TABDROP (WM_APP + 60)
#define WM_NC_TABCLOSE (WM_APP + 61)

/* drag_attach also reserves native label padding and paints close surfaces.
   No parent drawing/setup is required. Populate TCIF_PARAM with a stable Tab*
   on EVERY insert/refresh: close release validates both identity and geometry.
   WM_NC_TABCLOSE is synchronous: WPARAM=requested index, LPARAM=strip HWND.
   The parent owns document/prompt policy and may destroy the strip in response.
   Full visible hit rectangle in strip coordinates, FALSE if clipped/overflowed.
   Labels remain native/accessibility-readable; the X is a mouse decoration. */
BOOL drag_close_rect(HWND strip, int index, RECT *out);
typedef struct { HWND target; int index; POINT screen; } TabDrop;

/* GUI-thread only. Attach ordinary (not API-controlled) direct-child native tabs.
   The module never edits tabs/documents; the source editor handles WM_NC_TABDROP.
   WPARAM is the original source index; LPARAM is an opaque, synchronous token.
   drag_take's source is that editor frame, not its strip. A successful take
   consumes the token; it expires when SendMessage returns. No pointer is exposed.
   target is the destination editor frame, NULL for tear-off. index is an insertion
   position, adjusted for removal when reordering the same strip (0 for tear-off).
   screen comes from mouse-message coordinates, never the physical cursor. */
BOOL drag_attach(HWND strip);
BOOL drag_take(HWND source, LPARAM token, TabDrop *out);
#endif
