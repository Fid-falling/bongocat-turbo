/*
 * 崩溃兜底: 把"未处理异常"的异常码 + 出错模块名写进日志, 再交给系统默认处理。
 *
 * 为什么需要它: 用户看到的是"进程闪一下就消失"——没有弹窗、没有日志尾巴。Windows 默认
 * 的未处理异常会直接进 WER (多数机器上被关掉或静默), 于是"崩在哪个模块"这条最关键
 * 的线索完全丢失。这里装一个 SetUnhandledExceptionFilter:
 *   - 只记录, 不吞: 返回 EXCEPTION_CONTINUE_SEARCH, 让系统照常做它的默认处理
 *     (用户配置了转储仍然能拿到 dump)。改变崩溃行为会让"能不能现场调试"变得不可控;
 *   - 模块名来自异常地址反查 (GetModuleHandleExW 的 FROM_ADDRESS + GetModuleFileNameW),
 *     "崩在 nvoglv64 / amdvlk64 / bongo_live2d" 与 "崩在 BongoCatTurbo" 是两类完全不同
 *     的问题;
 *   - 日志写入走 bongo_cat_runtime_log_crash_line (无锁/不分配/写后 flush), 崩溃现场
 *     不能假设 SDL 的日志子系统还可用。
 *
 * 安装时机: 进程入口的最前面 (app_entry.c)。这样连"启动早期就崩"也能留下一条记录。
 */
#include "windows_diagnostics.h"

#ifdef _WIN32
#include "bongo_cat/log.h"

#include <stdio.h>
#include <string.h>

static void module_name_for(void *address, char *output, size_t capacity) {
    snprintf(output, capacity, "unknown");
    if (!address) return;
    HMODULE module = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)address, &module) || !module)
        return;
    wchar_t wide[MAX_PATH];
    if (!GetModuleFileNameW(module, wide, MAX_PATH)) return;
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, output, (int)capacity, NULL, NULL);
    output[capacity - 1] = '\0';
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *info) {
    DWORD code = 0;
    void *address = NULL;
    if (info && info->ExceptionRecord) {
        code = info->ExceptionRecord->ExceptionCode;
        address = info->ExceptionRecord->ExceptionAddress;
    }
    char module[MAX_PATH * 4];
    module_name_for(address, module, sizeof(module));
    char text[640];
    snprintf(text, sizeof(text),
        "崩溃 (未处理异常): 异常码=0x%08X 模块=%s 地址=%p 线程=%lu",
        (unsigned)code, module, address, (unsigned long)GetCurrentThreadId());
    bongo_cat_runtime_log_crash_line(text);
    /* 不吞异常: 系统照常做默认处理 (WER/转储), 我们只负责把证据留在日志里。 */
    return EXCEPTION_CONTINUE_SEARCH;
}

void bongo_cat_windows_install_crash_handler(void) {
    SetUnhandledExceptionFilter(crash_filter);
}

#endif /* _WIN32 */
