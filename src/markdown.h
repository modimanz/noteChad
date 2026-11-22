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
    MD_HEADING_NAVIGATION = 304,
    MD_READING_PREVIEW = 305
};
/* TRUE: recognized and handled; FALSE: unknown command or failure.
   UI-thread only. edit is a Unicode classic multiline EDIT owned by caller.
   Source text is never changed by navigation or preview. */
BOOL md_command(HWND owner, HWND edit, UINT id);
/* Close modeless views before a source is hidden, moved or destroyed. */
void md_forget(HWND edit);
#ifdef __cplusplus
}
#endif
#endif
