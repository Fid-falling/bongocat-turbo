#include "bongo_cat/platform.h"

#ifdef _WIN32
#include <stdlib.h>
#include <windows.h>
#include <shellapi.h>

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
