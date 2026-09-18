#include "bongo_cat/path.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include "windows_utf8.h"
#include <stdlib.h>
#include <windows.h>
#else
#include <dirent.h>
#include <errno.h>
#endif

bool bongo_cat_path_join(char *out, size_t cap, const char *left, const char *right) {
    if (!out || !cap || !left || !right) return false;
    size_t len = strlen(left);
    char sep = '/';
    bool has_sep = len > 0 && (left[len - 1] == '/' || left[len - 1] == '\\');
    int count = snprintf(out, cap, "%s%s%s", left, has_sep ? "" : (char[]){sep, 0}, right);
    return count >= 0 && (size_t)count < cap;
}

const char *bongo_cat_path_name(const char *path) {
    if (!path) return "";
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    const char *name = slash > backslash ? slash : backslash;
    return name ? name + 1 : path;
}

static bool path_type(const char *path, bool directory) {
#ifdef _WIN32
    wchar_t *wide = bongo_cat_windows_wide(path);
    DWORD attributes = wide ? GetFileAttributesW(wide) : INVALID_FILE_ATTRIBUTES;
    free(wide);
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (directory ? (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
            : (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0);
#else
    struct stat value;
    if (!path || stat(path, &value) != 0) return false;
    return directory ? S_ISDIR(value.st_mode) : S_ISREG(value.st_mode);
#endif
}

bool bongo_cat_path_is_file(const char *path) { return path_type(path, false); }
bool bongo_cat_path_is_dir(const char *path) { return path_type(path, true); }

bool bongo_cat_path_file_info(const char *path, uint64_t *size,
    uint64_t *modified) {
    if (!path || (!size && !modified)) return false;
#ifdef _WIN32
    wchar_t *wide = bongo_cat_windows_wide(path);
    WIN32_FILE_ATTRIBUTE_DATA info = {0};
    bool found = wide && GetFileAttributesExW(wide, GetFileExInfoStandard, &info) &&
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    free(wide);
    if (!found) return false;
    if (size) *size = ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    if (modified) *modified = ((uint64_t)info.ftLastWriteTime.dwHighDateTime << 32) |
        info.ftLastWriteTime.dwLowDateTime;
#else
    struct stat info;
    if (stat(path, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0) return false;
    if (size) *size = (uint64_t)info.st_size;
    if (modified) *modified = info.st_mtime < 0 ? 0 : (uint64_t)info.st_mtime;
#endif
    return true;
}

bool bongo_cat_path_file_size(const char *path, uint64_t *size) {
    return bongo_cat_path_file_info(path, size, NULL);
}

#ifdef _WIN32
static bool create_one(const wchar_t *path) {
    if (CreateDirectoryW(path, NULL)) return true;
    if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool bongo_cat_path_create_directory(const char *path) {
    wchar_t *wide = bongo_cat_windows_wide(path);
    if (!wide || !wide[0]) { free(wide); return false; }
    bool extended_unc = wcsncmp(wide, L"\\\\?\\UNC\\", 8) == 0;
    bool normal_unc = wide[0] == L'\\' && wide[1] == L'\\' && wide[2] != L'?';
    size_t start = extended_unc ? 8 : normal_unc ? 2 :
        wcsncmp(wide, L"\\\\?\\", 4) == 0 ? 7 : wide[1] == L':' ? 3 : 0;
    if (extended_unc || normal_unc) {
        wchar_t *server = wcschr(wide + start, L'\\');
        wchar_t *share = server ? wcschr(server + 1, L'\\') : NULL;
        start = share ? (size_t)(share - wide + 1) : wcslen(wide);
    }
    bool ok = true;
    for (wchar_t *cursor = wide + start; ok && *cursor; ++cursor) {
        if (*cursor != L'\\') continue;
        *cursor = L'\0'; ok = create_one(wide); *cursor = L'\\';
    }
    if (ok) ok = create_one(wide);
    free(wide); return ok;
}
#else
bool bongo_cat_path_create_directory(const char *path) {
    if (!path || !path[0]) return false;
    char copy[BONGO_CAT_PATH_CAP];
    int length = snprintf(copy, sizeof(copy), "%s", path);
    if (length < 0 || (size_t)length >= sizeof(copy)) return false;
    for (char *cursor = copy + 1; *cursor; ++cursor) {
        if (*cursor != '/') continue;
        *cursor = '\0';
        if (mkdir(copy, 0700) != 0 &&
            (errno != EEXIST || !bongo_cat_path_is_dir(copy))) return false;
        *cursor = '/';
    }
    return mkdir(copy, 0700) == 0 ||
        (errno == EEXIST && bongo_cat_path_is_dir(copy));
}
#endif

static bool ends_with(const char *text, const char *suffix) {
    size_t a = strlen(text), b = strlen(suffix);
    return a >= b && strcmp(text + a - b, suffix) == 0;
}

bool bongo_cat_path_find_suffix(const char *dir, const char *suffix, char *name, size_t cap) {
#ifdef _WIN32
    char pattern[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(pattern, sizeof(pattern), dir, "*")) return false;
    wchar_t *wide = bongo_cat_windows_wide(pattern);
    WIN32_FIND_DATAW data = {0};
    HANDLE find = wide ? FindFirstFileW(wide, &data) : INVALID_HANDLE_VALUE;
    free(wide);
    if (find == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        char filename[BONGO_CAT_PATH_CAP];
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            bongo_cat_windows_utf8(data.cFileName, filename, sizeof(filename)) &&
            ends_with(filename, suffix)) {
            snprintf(name, cap, "%s", filename);
            found = true;
            break;
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return found;
#else
    DIR *handle = opendir(dir);
    if (!handle) return false;
    bool found = false;
    struct dirent *entry;
    while ((entry = readdir(handle))) {
        if (ends_with(entry->d_name, suffix)) {
            snprintf(name, cap, "%s", entry->d_name);
            found = true;
            break;
        }
    }
    closedir(handle);
    return found;
#endif
}

int bongo_cat_path_find_unique_suffix(const char *dir, const char *suffix,
    char *name, size_t cap) {
#ifdef _WIN32
    char pattern[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(pattern, sizeof(pattern), dir, "*")) return 0;
    wchar_t *wide = bongo_cat_windows_wide(pattern);
    WIN32_FIND_DATAW data = {0};
    HANDLE find = wide ? FindFirstFileW(wide, &data) : INVALID_HANDLE_VALUE;
    free(wide);
    if (find == INVALID_HANDLE_VALUE) return 0;
    int count = 0;
    do {
        char filename[BONGO_CAT_PATH_CAP];
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            bongo_cat_windows_utf8(data.cFileName, filename, sizeof(filename)) &&
            ends_with(filename, suffix)) {
            if (++count == 1) snprintf(name, cap, "%s", filename);
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return count == 1 ? 1 : count ? -1 : 0;
#else
    DIR *handle = opendir(dir);
    if (!handle) return 0;
    int count = 0; struct dirent *entry;
    while ((entry = readdir(handle))) if (ends_with(entry->d_name, suffix)) {
        char path[BONGO_CAT_PATH_CAP];
        if (!bongo_cat_path_join(path, sizeof(path), dir, entry->d_name) ||
            !bongo_cat_path_is_file(path)) continue;
        if (++count == 1) snprintf(name, cap, "%s", entry->d_name);
    }
    closedir(handle);
    return count == 1 ? 1 : count ? -1 : 0;
#endif
}
