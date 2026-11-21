#ifndef NODECHAD_AI_H
#define NODECHAD_AI_H
#include <windows.h>
#define WM_NODECHAD_AI (WM_APP + 10)
void ai_toggle(HWND owner);
BOOL ai_enabled(void);
void ai_request(HWND owner, HWND edit, unsigned long long revision);
void ai_result(HWND owner, HWND edit, unsigned long long revision, LPARAM result);
#endif
