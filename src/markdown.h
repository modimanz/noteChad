#ifndef NOTECHAD_MARKDOWN_H
#define NOTECHAD_MARKDOWN_H
#include <windows.h>
#ifdef __cplusplus
extern "C" {
#endif
enum {
    MD_BOLD = 301,
    MD_INLINE_CODE = 302,
    MD_TASK = 303,
    MD_HEADING_NAVIGATION = 304
};
/* TRUE: recognized and handled; FALSE: unknown command or failure.
   UI-thread only. edit is a Unicode classic multiline EDIT owned by caller.
   Source text is never changed by navigation or preview. */
BOOL md_command(HWND owner, HWND edit, UINT id);
/* UI-thread only. Hidden, direct-child, read-only/copyable RichEdit; no focus
   change. Caller owns layout, visibility, key routing and destruction. */
HWND md_preview_create(HWND frame);
/* Fresh unsaved snapshot; never changes source selection, modify flag or undo.
   preserve_position retains/clamps pane selection and pixel scroll position.
   FALSE displays a nonmodal limitation/failure message for a valid pane.
   Bounded to 128K UTF-16 units, 2048 lines and a finite formatting-work budget.
   No networking or HTML/RTF interpretation. source must differ from pane. */
BOOL md_preview_update(HWND pane, HWND source, BOOL preserve_position);
/* Close the modeless outline for this source. Embedded panes are caller-owned. */
void md_forget(HWND edit);
#ifdef __cplusplus
}
#endif
#endif
