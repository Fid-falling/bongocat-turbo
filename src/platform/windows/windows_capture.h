#ifndef BONGO_CAT_WINDOWS_CAPTURE_H
#define BONGO_CAT_WINDOWS_CAPTURE_H

#ifdef _WIN32
#include <stdbool.h>
#include <windows.h>

/*
 * "让录屏软件(OBS)能抓到这只猫"所需的窗口属性管理。
 *
 * OBS 的窗口采集要求窗口能被系统枚举到、并且有 DWM 重定向表面, 所以这里围绕两件事:
 * 去掉 WS_EX_TOOLWINDOW(否则窗口根本不出现在采集列表里), 以及在样式变化之后把 DWM
 * 的透明合成重新建回来(样式事务会重建重定向表面)。
 * 另外两件杂事: 用 ITaskbarList 把窗口从任务栏列表里摘掉, 以及响应资源管理器重启
 * (TaskbarCreated 广播)后重新摘一次。
 * 所有函数都假定调用方在窗口所属线程上(它们会改窗口样式并投递消息)。
 */
bool bongo_cat_windows_capture_configure(HWND window);
/* Mark windows whose OpenGL back buffer relies on DWM alpha composition. */
void bongo_cat_windows_capture_mark_transparent(HWND window, bool enabled);
/* Returns whether OBS/capture discovery has been configured for this HWND. */
bool bongo_cat_windows_capture_is_configured(HWND window);
/* Install a small native shim for synchronous DWM/shell notifications. */
void bongo_cat_windows_capture_install_transparency_handler(HWND window);
bool bongo_cat_windows_capture_restore_transparency(HWND window);
/* Re-apply DWM alpha composition after a shell, style, or display change. */
void bongo_cat_windows_capture_repair_transparency(HWND window);
bool bongo_cat_windows_capture_handle_transparency_message(
    HWND window, UINT message, WPARAM wparam);
void bongo_cat_windows_capture_log(HWND window, const char *stage);
bool bongo_cat_windows_capture_handle_message(
    HWND window, UINT message, WPARAM wparam);
#endif

#endif
