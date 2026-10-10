#include "bongo_cat/file.h"

/*
 * 跨平台文件原语: 打开 / 追加 / 删除 / 原子替换。
 *
 * 为什么不直接用标准库: Windows 上路径是 UTF-16, 必须走 _wfopen 和 *W 系列
 * API, 否则中文用户名之类的非 ASCII 目录会直接失败; 追加写还要先独占加锁再
 * 定位到文件末尾, 否则多实例同时写同一个日志会互相覆盖。
 * durable 表示是否要求真正落盘(Windows 用 MOVEFILE_WRITE_THROUGH), POSIX 侧
 * 没有等价开关, 由调用方自己 fsync(见 config_file.c)。
 */
#ifdef _WIN32
#include "windows_utf8.h"
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* fopen 的 mode 串只可能是 ASCII, 逐字节展宽即可; 长度 >= 16 说明调用方传了
   奇怪的东西, 直接失败而不是截断。 */
static bool wide_mode(const char *mode, wchar_t output[16]) {
    if (!mode || strlen(mode) >= 16) return false;
    size_t index = 0;
    for (; mode[index]; ++index) output[index] = (wchar_t)(unsigned char)mode[index];
    output[index] = L'\0'; return true;
}

FILE *bongo_cat_file_open(const char *path, const char *mode) {
/* 追加写: 先 LockFileEx 独占, 再定位到文件末尾, 写完解锁。加锁是为了多实例
   同时写日志时不互相覆盖; 锁失败就整体失败, 不允许"不加锁地写一半"。 */
    wchar_t mode_wide[16];
    wchar_t *path_wide = bongo_cat_windows_wide(path);
    FILE *file = path_wide && wide_mode(mode, mode_wide)
        ? _wfopen(path_wide, mode_wide) : NULL;
    free(path_wide); return file;
}

bool bongo_cat_file_append(const char *path, const void *data, size_t size) {
    if (!path || !path[0] || (!data && size)) return false;
    wchar_t *wide = bongo_cat_windows_wide(path);
    HANDLE file = wide ? CreateFileW(wide, GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
    free(wide);
    if (file == INVALID_HANDLE_VALUE) return false;
    OVERLAPPED lock = {0};
    bool locked = LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK, 0,
        MAXDWORD, MAXDWORD, &lock) != FALSE;
    LARGE_INTEGER end = {0};
    bool positioned = locked && SetFilePointerEx(file, end, NULL,
        FILE_END) != FALSE;
    const unsigned char *cursor = data;
    size_t remaining = size;
    bool written = positioned;
    while (written && remaining) {
        DWORD chunk = remaining > MAXDWORD ? MAXDWORD : (DWORD)remaining;
        DWORD count = 0;
        written = WriteFile(file, cursor, chunk, &count, NULL) != FALSE &&
            count > 0;
        cursor += count;
        remaining -= count;
    }
    if (locked && !UnlockFileEx(file, 0, MAXDWORD, MAXDWORD, &lock))
        written = false;
    if (!CloseHandle(file)) written = false;
    return written;
}

bool bongo_cat_file_remove(const char *path) {
    wchar_t *wide = bongo_cat_windows_wide(path);
    bool removed = wide && _wremove(wide) == 0;
    free(wide); return removed;
}

bool bongo_cat_file_replace(const char *source, const char *target, bool durable) {
    wchar_t *source_wide = bongo_cat_windows_wide(source);
    wchar_t *target_wide = bongo_cat_windows_wide(target);
/* MOVEFILE_WRITE_THROUGH 让替换本身等到元数据也落盘; durable=false 表示调用
   方认为这次的文件丢了也无所谓。 */
    DWORD flags = MOVEFILE_REPLACE_EXISTING | (durable ? MOVEFILE_WRITE_THROUGH : 0);
    bool replaced = source_wide && target_wide &&
        MoveFileExW(source_wide, target_wide, flags) != FALSE;
    free(source_wide); free(target_wide); return replaced;
}
#else
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/file.h>
#include <unistd.h>

FILE *bongo_cat_file_open(const char *path, const char *mode) { return fopen(path, mode); }

/* POSIX 侧对应实现: flock + O_APPEND。EINTR 必须重试 —— 被信号打断不等于
   加锁失败, 直接放弃会丢掉并发下的写保护。 */
static bool lock_file(int file, int operation) {
    int result;
    do result = flock(file, operation); while (result != 0 && errno == EINTR);
    return result == 0;
}

bool bongo_cat_file_append(const char *path, const void *data, size_t size) {
    if (!path || !path[0] || (!data && size)) return false;
    int file = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (file < 0) return false;
    bool locked = lock_file(file, LOCK_EX);
    const unsigned char *cursor = data;
    size_t remaining = size;
    bool written = locked;
    while (written && remaining) {
        ssize_t count = write(file, cursor, remaining);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) written = false;
        else { cursor += count; remaining -= (size_t)count; }
    }
    if (locked && !lock_file(file, LOCK_UN)) written = false;
    if (close(file) != 0) written = false;
    return written;
}
bool bongo_cat_file_remove(const char *path) { return remove(path) == 0; }
bool bongo_cat_file_replace(const char *source, const char *target, bool durable) {
/* POSIX 的 rename 本身就是原子的, 没有等价的"写穿"开关, 真正要求落盘的
   调用方会自己 fsync(见 config_file.c 的 sync_file / sync_parent)。 */
    (void)durable; return rename(source, target) == 0;
}
#endif
