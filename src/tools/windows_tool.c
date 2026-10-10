#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "windows_tool.h"

#include <stdlib.h>
#include <string.h>

/*
 * 工具程序共用的 Windows 小工具: 逐级创建目录。
 *
 * 与 src/core/path.c 里的同名实现是两套代码 —— 工具刻意不链接产品代码, 免得把
 * 运行时依赖带进构建期脚本, 所以这里只保留最小实现: 按分隔符切开逐个
 * CreateDirectory。已经存在(ERROR_ALREADY_EXISTS)不算失败, 保证可以重复调用。
 */
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
        /* 跳过开头的分隔符(UNC 的 "\\")和盘符后面的那一个 ':' 位置: 对 "C:"
           或 "\\" 调 CreateDirectory 注定失败, 而且它们也不是有意义的目录层级。 */
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
