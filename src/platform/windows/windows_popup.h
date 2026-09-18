#ifndef BONGO_CAT_WINDOWS_POPUP_H
#define BONGO_CAT_WINDOWS_POPUP_H

#ifdef _WIN32
#include <windows.h>

UINT bongo_cat_windows_popup_track(HWND owner, HMENU menu, UINT flags,
    int x, int y);
void bongo_cat_windows_popup_complete(HWND owner);
#endif

#endif
