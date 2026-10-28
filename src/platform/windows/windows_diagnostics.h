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

/*
 * 自检 (设置 → 应用 → 运行自检) 要用的三个系统事实, 实现在 windows_selfcheck.c。
 * 通用自检代码只看到"拿到/拿不到", 不直接碰 Win32。
 *   - os:     "Windows 11 build 26100 (10.0.26100)"; 失败返回 false
 *   - arch:   "x64" / "arm64" / "x86"; 恒有返回值 (不会返回 NULL)
 *   - disk:   path 所在卷的可用字节与总字节; 失败返回 false
 */
bool bongo_cat_windows_selfcheck_os(char *output, size_t capacity);
const char *bongo_cat_windows_selfcheck_arch(void);
bool bongo_cat_windows_selfcheck_disk(const char *path,
    unsigned long long *free_bytes, unsigned long long *total_bytes);

/*
 * 崩溃兜底 (实现在 windows_crash.c): 装一个不吞异常的 UnhandledExceptionFilter,
 * 把异常码 + 出错模块名写进日志后交回系统默认处理。进程入口最前面调用一次即可。
 */
void bongo_cat_windows_install_crash_handler(void);
#endif

#endif
