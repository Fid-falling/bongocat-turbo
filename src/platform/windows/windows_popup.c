#include "windows_popup.h"

#ifdef _WIN32
void bongo_cat_windows_popup_complete(HWND owner) {
    if (owner) PostMessageW(owner, WM_NULL, 0, 0);
}

UINT bongo_cat_windows_popup_track(HWND owner, HMENU menu, UINT flags,
    int x, int y) {
    if (!owner || !menu) return 0;
    SetForegroundWindow(owner);
    UINT command = TrackPopupMenu(menu, flags, x, y, 0, owner, NULL);
    bongo_cat_windows_popup_complete(owner);
    return command;
}
#endif
