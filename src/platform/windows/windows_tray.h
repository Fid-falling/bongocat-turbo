#ifndef BONGO_CAT_WINDOWS_TRAY_H
#define BONGO_CAT_WINDOWS_TRAY_H

#ifdef _WIN32
#include <windows.h>

void bongo_cat_windows_tray_handle_message(UINT message);
#endif

#endif
