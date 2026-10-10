#ifndef BONGO_CAT_WINDOWS_POPUP_H
#define BONGO_CAT_WINDOWS_POPUP_H

#ifdef _WIN32
#include <windows.h>

/*
 * 任务栏弹出菜单的封装。
 *
 * TrackPopupMenu 有两个必须成对完成的动作, 少一个就会出现"菜单点完不消失"或者"焦点
 * 留在菜单上"这类经典问题: 调用前 SetForegroundWindow(owner), 调用后往 owner 投一条
 * WM_NULL。这两个动作由本模块负责, 调用方不要再自己做一遍。
 */
UINT bongo_cat_windows_popup_track(HWND owner, HMENU menu, UINT flags,
    int x, int y);
void bongo_cat_windows_popup_complete(HWND owner);
#endif

#endif
