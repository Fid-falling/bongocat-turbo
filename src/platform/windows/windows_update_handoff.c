#include "windows_startup.h"
#include "bongo_cat/update.h"

#ifdef _WIN32
#include <windows.h>

/*
 * 更新交棒: "新进程请旧进程让位"。
 *
 * 只有在新进程版本不低于旧进程、并且旧进程确实是同一个可执行文件时才动手 —— 否则
 * 用户可能只是想把便携版和安装版一起打开, 那时不该把已经在跑的那个关掉。
 * 版本与路径都来自共享内存(见 windows_startup.c); 只要旧实例没写出共享内存就一律
 * 放弃: 宁可多开一个, 也不能误杀。
 */
static bool request_shutdown(HWND existing) {
    HANDLE stopped = OpenEventW(SYNCHRONIZE, FALSE,
        bongo_cat_windows_instance_stopped_name());
    HANDLE shutdown = OpenEventW(EVENT_MODIFY_STATE, FALSE,
        bongo_cat_windows_update_shutdown_name());
    if (!stopped || !shutdown || !existing) {
        if (stopped) CloseHandle(stopped);
        if (shutdown) CloseHandle(shutdown);
        return false;
    }
    bool signaled = SetEvent(shutdown) != 0;
    if (signaled) PostMessageW(existing, WM_CLOSE, 0, 0);
    DWORD result = signaled ? WaitForSingleObject(stopped, 15000) : WAIT_FAILED;
    CloseHandle(shutdown);
    CloseHandle(stopped);
    return result == WAIT_OBJECT_0;
}

/* 先看有没有旧实例, 再读它的共享内存信息, 然后按"是不是同一个 exe + 版本谁更新"
   决定是否请求它退出。三个事件里 wake 用不上, 这里只用到 update-shutdown 与
   stopped。 */
bool bongo_cat_windows_update_handoff(void) {
    HWND existing = FindWindowW(NULL, bongo_cat_windows_instance_title());
    HANDLE mapping = existing ? OpenFileMappingW(FILE_MAP_READ, FALSE,
        bongo_cat_windows_instance_info_name()) : NULL;
    BongoCatWindowsInstanceInfo info = {0};
    BongoCatWindowsInstanceInfo *shared = mapping ? MapViewOfFile(mapping,
        FILE_MAP_READ, 0, 0, sizeof(info)) : NULL;
    if (shared) info = *shared;
    if (shared) UnmapViewOfFile(shared);
    if (mapping) CloseHandle(mapping);
    wchar_t current_path[BONGO_CAT_WINDOWS_INSTANCE_PATH_CAP] = {0};
    DWORD current_length = GetModuleFileNameW(NULL, current_path,
        BONGO_CAT_WINDOWS_INSTANCE_PATH_CAP);
    if (!existing || !current_length || current_length >=
            BONGO_CAT_WINDOWS_INSTANCE_PATH_CAP ||
        (info.magic == 0x42434D49u &&
            (_wcsicmp(current_path, info.executable_path) == 0 ||
                !info.version[0] || bongo_cat_update_compare_versions(
                    BONGO_CAT_VERSION, info.version) <= 0)))
        return false;
    if (info.magic != 0x42434D49u || !info.version[0]) return false;
    return request_shutdown(existing);
}
#endif
