#include "bongo_cat/platform.h"

#ifdef _WIN32
#include <stdlib.h>
#include <windows.h>
#include <shellapi.h>

/*
 * 用资源管理器打开一个目录(设置界面里的"打开模型文件夹")。
 *
 * ShellExecuteW 的返回值必须 > 32 才算成功: 它的返回类型是 HINSTANCE, 但小于等于
 * 32 的那些值其实是错误码(这是 Win32 的历史约定, 不看这一条就会把失败当成功)。
 * 路径是 UTF-8, 必须显式转成宽字符; MB_ERR_INVALID_CHARS 让非法 UTF-8 直接失败,
 * 而不是被替换成 '?' —— 路径里多一个 '?' 会指向完全不同的文件。
 */
bool bongo_cat_platform_open_directory(const char *path) {
    if (!path || !path[0]) return false;
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        path, -1, NULL, 0);
    wchar_t *directory = length > 0 ?
        malloc((size_t)length * sizeof(*directory)) : NULL;
    if (!directory || !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        path, -1, directory, length)) {
        free(directory);
        return false;
    }
    HINSTANCE result = ShellExecuteW(NULL, L"open", directory,
        NULL, NULL, SW_SHOWNORMAL);
    free(directory);
    return (INT_PTR)result > 32;
}
#endif
