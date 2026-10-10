#ifndef BONGO_CAT_WINDOWS_TRAY_H
#define BONGO_CAT_WINDOWS_TRAY_H

#ifdef _WIN32
#include <windows.h>

/*
 * 托盘图标在 Windows 上的两件"接线"工作。
 *
 * SDL 的托盘实现内部用一个 message-only 窗口接收 Shell_NotifyIcon 的回调, 却不给应用
 * 挂自己窗口过程的机会, 所以这里先找到那个窗口再把过程链上去(见 .c), 才拿得到左键
 * 单击与右键菜单事件。
 * 另一件事: 资源管理器重启后托盘图标会消失, 需要响应 TaskbarCreated 广播重建。
 */
void bongo_cat_windows_tray_handle_message(UINT message);
#endif

#endif
