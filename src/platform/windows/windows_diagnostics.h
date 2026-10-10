#ifndef BONGO_CAT_WINDOWS_DIAGNOSTICS_H
#define BONGO_CAT_WINDOWS_DIAGNOSTICS_H

#ifdef _WIN32
#include <stdbool.h>
#include <windows.h>

/*
 * 采集诊断探针(实现在 windows_capture_probe.c)。frame_presentation_audit 会在
 * 两个阶段调用它: "opengl-source"(SDL 的源窗口)与 "layered-proxy"(分层窗口的
 * 代理窗口), 用来确认录屏软件到底能不能抓到这块画面。
 * stage 只用于日志, 调用方可以传任意常量字符串。
 */
bool bongo_cat_windows_diagnostics_probe_capture(
    HWND window, const char *stage);
#endif

#endif
