#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "cubism_viewer_blind_frames.h"

#include <stdlib.h>
#include <string.h>

wchar_t *blind_join_path(const wchar_t *directory, const wchar_t *name) {
    size_t a = wcslen(directory), b = wcslen(name);
    wchar_t *result = (wchar_t *)malloc((a + b + 2) * sizeof(*result));
    if (!result) return NULL;
    memcpy(result, directory, a * sizeof(*result));
    result[a] = L'\\';
    memcpy(result + a + 1, name, (b + 1) * sizeof(*result));
    return result;
}

wchar_t *blind_join_suffix(const wchar_t *directory, const wchar_t *name,
    const wchar_t *suffix) {
    size_t a = wcslen(directory), b = wcslen(name), c = wcslen(suffix);
    wchar_t *result = (wchar_t *)malloc((a + b + c + 3) * sizeof(*result));
    if (!result) return NULL;
    swprintf(result, a + b + c + 3, L"%ls\\%ls%ls", directory, name, suffix);
    return result;
}

static int reserve_frames(FrameList *list) {
    size_t capacity = list->capacity ? list->capacity * 2 : 32;
    Frame *items;
    if (capacity < list->capacity || capacity > SIZE_MAX / sizeof(*items)) return 0;
    items = (Frame *)realloc(list->items, capacity * sizeof(*items));
    if (!items) return 0;
    list->items = items; list->capacity = capacity;
    return 1;
}

void blind_free_frames(FrameList *list) {
    size_t index;
    for (index = 0; index < list->count; ++index) {
        free(list->items[index].name);
        free(list->items[index].viewer_path);
        free(list->items[index].native_path);
        bongo_cat_validation_image_free(&list->items[index].viewer);
        bongo_cat_validation_image_free(&list->items[index].native);
    }
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static wchar_t *without_extension(const wchar_t *name) {
    const wchar_t *dot = wcsrchr(name, L'.');
    size_t length = dot ? (size_t)(dot - name) : wcslen(name);
    wchar_t *result = (wchar_t *)malloc((length + 1) * sizeof(*result));
    if (result) { memcpy(result, name, length * sizeof(*result)); result[length] = L'\0'; }
    return result;
}

static wchar_t *find_native(const wchar_t *directory, const wchar_t *name) {
    static const wchar_t *extensions[] = {L".bmp", L".png"};
    size_t index;
    for (index = 0; index < sizeof(extensions) / sizeof(extensions[0]); ++index) {
        wchar_t *path = blind_join_suffix(directory, name, extensions[index]);
        if (!path) return NULL;
        if (_waccess(path, 0) == 0) return path;
        free(path);
    }
    return NULL;
}

static int compare_frames(const void *left, const void *right) {
    const Frame *a = (const Frame *)left, *b = (const Frame *)right;
    return wcscmp(a->name, b->name);
}

int blind_collect_frames(const wchar_t *viewer_directory,
    const wchar_t *native_directory, FrameList *list) {
    WIN32_FIND_DATAW entry;
    HANDLE find;
    wchar_t *pattern;
    static const wchar_t *extensions[] = {L"*.png", L"*.bmp"};
    size_t extension;
    for (extension = 0; extension < sizeof(extensions) / sizeof(extensions[0]); ++extension) {
        pattern = blind_join_path(viewer_directory, extensions[extension]);
        if (!pattern) return 0;
        find = FindFirstFileW(pattern, &entry);
        free(pattern);
        if (find == INVALID_HANDLE_VALUE) continue;
        do {
            wchar_t *name, *viewer_path, *native_path;
            Frame *frame;
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            name = without_extension(entry.cFileName);
            viewer_path = blind_join_path(viewer_directory, entry.cFileName);
            native_path = name ? find_native(native_directory, name) : NULL;
            if (!name || !viewer_path) {
                free(name); free(viewer_path); free(native_path);
                FindClose(find); return 0;
            }
            if (!native_path) { free(name); free(viewer_path); continue; }
            if (list->count == list->capacity && !reserve_frames(list)) {
                free(name); free(viewer_path); free(native_path);
                FindClose(find); return 0;
            }
            frame = &list->items[list->count++];
            memset(frame, 0, sizeof(*frame));
            frame->name = name; frame->viewer_path = viewer_path;
            frame->native_path = native_path;
        } while (FindNextFileW(find, &entry));
        FindClose(find);
    }
    qsort(list->items, list->count, sizeof(*list->items), compare_frames);
    return list->count != 0;
}
