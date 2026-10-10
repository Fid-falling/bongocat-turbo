#include "bongo_cat/path.h"

#include <stdio.h>
/*
 * 目录遍历与文件搬运(枚举 / 复制 / 改名 / 删除), 声明在 bongo_cat/path.h。
 *
 * 契约: visitor 返回 FAILURE 表示调用方自己出错(枚举整体返回 false); 返回
 * SUCCESS 表示"够了, 提前停下", 此时枚举仍然算成功。路径同样是 UTF-8,
 * Windows 侧转宽字符后再调用 *W API。
 */
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include "windows_utf8.h"
#include <windows.h>
#else
#include <dirent.h>
#include <errno.h>
#endif

#ifdef _WIN32
bool bongo_cat_path_enumerate(const char *path,
    BongoCatPathVisitor visitor, void *userdata) {
/* 空目录在 Windows 上会让 FindFirstFileW 返回 ERROR_FILE_NOT_FOUND, 而不是
   "0 个结果"; 所以这个错误要单独放行: 只要目录确实存在就当成空目录。 */
    char pattern[BONGO_CAT_PATH_CAP];
    if (!path || !visitor ||
        !bongo_cat_path_join(pattern, sizeof(pattern), path, "*")) return false;
    wchar_t *wide = bongo_cat_windows_wide(pattern);
    WIN32_FIND_DATAW data = {0};
    HANDLE find = wide ? FindFirstFileW(wide, &data) : INVALID_HANDLE_VALUE;
    free(wide);
    if (find == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND && bongo_cat_path_is_dir(path);
    bool ok = true, stopped = false;
    do {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0)
            continue;
        char name[BONGO_CAT_PATH_CAP];
        if (!bongo_cat_windows_utf8(data.cFileName, name, sizeof(name))) {
            ok = false; break;
        }
        BongoCatPathVisit result = visitor(userdata, path, name);
        if (result == BONGO_CAT_PATH_FAILURE) { ok = false; break; }
        if (result == BONGO_CAT_PATH_SUCCESS) { stopped = true; break; }
    } while (FindNextFileW(find, &data));
    if (!stopped && ok && GetLastError() != ERROR_NO_MORE_FILES) ok = false;
    FindClose(find);
    return ok;
}

bool bongo_cat_path_copy_file(const char *source, const char *target) {
    wchar_t *wide_source = bongo_cat_windows_wide(source);
    wchar_t *wide_target = bongo_cat_windows_wide(target);
/* 第三个参数 FALSE = 允许覆盖已存在的目标。上层用它把下载好的文件搬到最终
   位置, 因此覆盖是预期行为。 */
    bool ok = wide_source && wide_target && CopyFileW(wide_source, wide_target, FALSE);
    free(wide_source); free(wide_target); return ok;
}

bool bongo_cat_path_rename(const char *source, const char *target) {
    wchar_t *wide_source = bongo_cat_windows_wide(source);
    wchar_t *wide_target = bongo_cat_windows_wide(target);
    bool ok = wide_source && wide_target && MoveFileExW(wide_source, wide_target,
/* 覆盖 + 写穿: 改名要么完整生效、要么完全不生效, 中途掉电不会留下两个文件
   或者半个文件。 */
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    free(wide_source); free(wide_target); return ok;
}

bool bongo_cat_path_remove(const char *path) {
    wchar_t *wide = bongo_cat_windows_wide(path);
/* Windows 删目录和删文件是两套 API, 所以先用属性位判断类型再分发。 */
    DWORD attributes = wide ? GetFileAttributesW(wide) : INVALID_FILE_ATTRIBUTES;
    bool ok = attributes != INVALID_FILE_ATTRIBUTES &&
        ((attributes & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(wide) : DeleteFileW(wide));
    free(wide); return ok;
}
#else
bool bongo_cat_path_enumerate(const char *path,
    BongoCatPathVisitor visitor, void *userdata) {
    if (!path || !visitor) return false;
/* readdir 要"返回值 + errno"双条件判断结束: 必须先清零 errno, 返回 NULL 且
   errno 非 0 才是真出错, 否则只是读到了目录末尾。 */
    DIR *directory = opendir(path);
    if (!directory) return false;
    bool ok = true;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry) { if (errno) ok = false; break; }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        BongoCatPathVisit result = visitor(userdata, path, entry->d_name);
        if (result == BONGO_CAT_PATH_FAILURE) { ok = false; break; }
        if (result == BONGO_CAT_PATH_SUCCESS) break;
    }
    if (closedir(directory) != 0) ok = false;
    return ok;
}

bool bongo_cat_path_copy_file(const char *source, const char *target) {
/* 同名复制直接拒绝 —— 一边读一边写同一个文件会把内容毁掉。
   失败时删掉写了一半的目标文件, 不留下半成品。 */
    if (!source || !target || strcmp(source, target) == 0) return false;
    FILE *input = fopen(source, "rb"), *output = input ? fopen(target, "wb") : NULL;
    if (!output) { if (input) fclose(input); return false; }
    unsigned char buffer[8192]; bool ok = true; size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), input)) > 0)
        if (fwrite(buffer, 1, count, output) != count) { ok = false; break; }
    if (ferror(input)) ok = false;
    if (fclose(input) != 0) ok = false;
    if (fclose(output) != 0) ok = false;
    if (!ok) remove(target);
    return ok;
}

bool bongo_cat_path_rename(const char *source, const char *target) {
    return source && target && rename(source, target) == 0;
}

bool bongo_cat_path_remove(const char *path) {
    return path && remove(path) == 0;
}
#endif
