/*
 * 自检需要的三个 Windows 系统事实: OS 版本、进程架构、卷剩余空间。
 *
 * 为什么不放在自检的通用代码里: 这三个都只能靠 Win32 API (RtlGetVersion /
 * GetNativeSystemInfo / GetDiskFreeSpaceExW), 而自检的其余部分是跨平台的。把它们留在
 * 平台层, 通用代码只看到"能拿到/拿不到"的布尔与字符串。
 *
 * 版本来源用 RtlGetVersion 而不是 GetVersionExW: 后者受兼容性清单影响, 在 Win10+ 上
 * 会谎报 6.2 —— 报告里的系统版本是给支持判断"是不是 Win10 的老驱动问题"用的, 谎报
 * 比没有更糟。
 */
#include "windows_diagnostics.h"

#ifdef _WIN32
#include <stdio.h>
#include <string.h>

/* 只取函数地址: 链接 ntdll 会增加导入表项, 而这里只需要一个只读查询 */
typedef LONG (WINAPI *BongoCatRtlGetVersion)(PRTL_OSVERSIONINFOW);

static const char *architecture_name(WORD architecture) {
    switch (architecture) {
    case PROCESSOR_ARCHITECTURE_AMD64: return "x64";
    case PROCESSOR_ARCHITECTURE_ARM64: return "arm64";
    case PROCESSOR_ARCHITECTURE_INTEL: return "x86";
    case PROCESSOR_ARCHITECTURE_ARM: return "arm";
    default: return "unknown";
    }
}

const char *bongo_cat_windows_selfcheck_arch(void) {
    SYSTEM_INFO info;
    GetNativeSystemInfo(&info);
    return architecture_name(info.wProcessorArchitecture);
}

bool bongo_cat_windows_selfcheck_os(char *output, size_t capacity) {
    if (!output || capacity == 0) return false;
    RTL_OSVERSIONINFOW version;
    memset(&version, 0, sizeof(version));
    version.dwOSVersionInfoSize = sizeof(version);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    BongoCatRtlGetVersion query = ntdll ? (BongoCatRtlGetVersion)(void *)
        GetProcAddress(ntdll, "RtlGetVersion") : NULL;
    if (!query || query(&version) != 0) return false;
    /* Windows 11 与 Windows 10 的 major.minor 都是 10.0: 用 build 号区分
       (22000 起是 Win11), 否则报告会把用户报的 Win11 显示成 Win10。 */
    const char *name = "Windows";
    if (version.dwMajorVersion == 10 && version.dwMinorVersion == 0)
        name = version.dwBuildNumber >= 22000 ? "Windows 11" : "Windows 10";
    else if (version.dwMajorVersion == 6 && version.dwMinorVersion == 3)
        name = "Windows 8.1";
    else if (version.dwMajorVersion == 6 && version.dwMinorVersion == 2)
        name = "Windows 8";
    else if (version.dwMajorVersion == 6 && version.dwMinorVersion == 1)
        name = "Windows 7";
    snprintf(output, capacity, "%s build %lu (%lu.%lu.%lu)", name,
        (unsigned long)version.dwBuildNumber,
        (unsigned long)version.dwMajorVersion,
        (unsigned long)version.dwMinorVersion,
        (unsigned long)version.dwBuildNumber);
    return true;
}

bool bongo_cat_windows_selfcheck_disk(const char *path,
    unsigned long long *free_bytes, unsigned long long *total_bytes) {
    if (!path || !path[0] || !free_bytes || !total_bytes) return false;
    wchar_t wide[32768];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide,
            32768)) return false;
    ULARGE_INTEGER free_for_caller, total, free_total;
    if (!GetDiskFreeSpaceExW(wide, &free_for_caller, &total, &free_total))
        return false;
    *free_bytes = (unsigned long long)free_for_caller.QuadPart;
    *total_bytes = (unsigned long long)total.QuadPart;
    return true;
}

#endif /* _WIN32 */
