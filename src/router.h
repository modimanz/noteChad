#pragma once
#include <windows.h>
/* Regular launches of the same installed binary share one UI thread. Automation
   deliberately bypasses the router; no note content or edit handles cross IPC. */
enum RouterResult { ROUTER_PRIMARY=0, ROUTER_FORWARDED=1, ROUTER_ERROR=2 };
typedef void (*RouterOpen)(int count,const WCHAR *const *paths);
int router_start(int argc,WCHAR **argv,RouterOpen open_window);
void router_stop(void);
