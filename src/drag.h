#ifndef NOTECHAD_DRAG_H
#define NOTECHAD_DRAG_H
#include <windows.h>

#define WM_NC_TABDROP (WM_APP + 60)
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
