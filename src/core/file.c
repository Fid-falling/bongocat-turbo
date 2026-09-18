#include "bongo_cat/file.h"

#ifdef _WIN32
#include "windows_utf8.h"
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static bool wide_mode(const char *mode, wchar_t output[16]) {
    if (!mode || strlen(mode) >= 16) return false;
    size_t index = 0;
    for (; mode[index]; ++index) output[index] = (wchar_t)(unsigned char)mode[index];
    output[index] = L'\0'; return true;
}

FILE *bongo_cat_file_open(const char *path, const char *mode) {
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
    (void)durable; return rename(source, target) == 0;
}
#endif
