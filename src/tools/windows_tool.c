#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "windows_tool.h"

#include <stdlib.h>
#include <string.h>

int bongo_cat_tool_ensure_directory(const wchar_t *path) {
    size_t length;
    wchar_t *copy, *cursor;
    int ok = 0;
    if (!path || !*path) return 0;
    length = wcslen(path);
    copy = (wchar_t *)malloc((length + 1) * sizeof(*copy));
    if (!copy) return 0;
    memcpy(copy, path, (length + 1) * sizeof(*copy));
    for (cursor = copy; *cursor; ++cursor) if (*cursor == L'/' || *cursor == L'\\') {
        wchar_t saved = *cursor;
        if (cursor == copy || (cursor == copy + 2 && copy[1] == L':')) continue;
        *cursor = L'\0';
        if (!CreateDirectoryW(copy, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
            goto done;
        *cursor = saved;
    }
    if (!CreateDirectoryW(copy, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        goto done;
    ok = 1;
done:
    free(copy);
    return ok;
}
