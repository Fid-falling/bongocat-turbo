#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "mver_phase_frames.h"
#include "validation_image.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* 只用 '\\' 拼接: 这些工具只在 Windows 上构建, 参数也都是自己传的, 不需要
   兼容另一种分隔符。 */
static wchar_t *join_path(const wchar_t *directory, const wchar_t *name) {
    size_t a = wcslen(directory), b = wcslen(name);
    wchar_t *result = (wchar_t *)malloc((a + b + 2) * sizeof(*result));
    if (result) swprintf(result, a + b + 2, L"%ls\\%ls", directory, name);
    return result;
}

static int reserve_paths(PathList *list) {
    size_t capacity = list->capacity ? list->capacity * 2 : 32;
    wchar_t **items;
    if (capacity < list->capacity || capacity > SIZE_MAX / sizeof(*items)) return 0;
    items = (wchar_t **)realloc(list->items, capacity * sizeof(*items));
    if (!items) return 0;
    list->items = items; list->capacity = capacity;
    return 1;
}

void phase_free_paths(PathList *list) {
    size_t index;
    for (index = 0; index < list->count; ++index) free(list->items[index]);
    free(list->items); memset(list, 0, sizeof(*list));
}

static int compare_paths(const void *left, const void *right) {
    return wcscmp(*(const wchar_t *const *)left, *(const wchar_t *const *)right);
}

/* 把 *.png 和 *.bmp 分两轮收齐之后再统一按文件名排序 —— 排序必须在两轮之后
   做, 否则帧序会按扩展名分组, 相位比对就失去意义了。目录为空返回 0。 */
int phase_collect_paths(const wchar_t *directory, PathList *list) {
    static const wchar_t *patterns[] = {L"*.png", L"*.bmp"};
    size_t pattern_index;
    for (pattern_index = 0; pattern_index < sizeof(patterns) / sizeof(patterns[0]);
        ++pattern_index) {
        WIN32_FIND_DATAW entry;
        wchar_t *pattern = join_path(directory, patterns[pattern_index]);
        HANDLE find;
        if (!pattern) return 0;
        find = FindFirstFileW(pattern, &entry); free(pattern);
        if (find == INVALID_HANDLE_VALUE) continue;
        do {
            wchar_t *path;
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            path = join_path(directory, entry.cFileName);
            if (!path || (list->count == list->capacity && !reserve_paths(list))) {
                free(path); FindClose(find); return 0;
            }
            list->items[list->count++] = path;
        } while (FindNextFileW(find, &entry));
        FindClose(find);
    }
    qsort(list->items, list->count, sizeof(*list->items), compare_paths);
    return list->count != 0;
}

/* 按 PIXEL_STEP 抽样成网格并转成紧凑的 BGR(丢掉 alpha), 后续比对只看颜色。
   抽样坐标必须钳到 width-1 / height-1: 尺寸不是 4 的整数倍时最后一格会越界。 */
static int load_frame(const wchar_t *path, Frame *frame) {
    BongoCatValidationImage image = {0};
    int gx, gy;
    if (!bongo_cat_validation_image_load(path, &image)) return 0;
    frame->width = image.width; frame->height = image.height;
    frame->grid_width = (image.width + PIXEL_STEP - 1) / PIXEL_STEP;
    frame->grid_height = (image.height + PIXEL_STEP - 1) / PIXEL_STEP;
    frame->bgr = (unsigned char *)malloc((size_t)frame->grid_width *
        frame->grid_height * 3);
    if (!frame->bgr) { bongo_cat_validation_image_free(&image); return 0; }
    for (gy = 0; gy < frame->grid_height; ++gy)
    for (gx = 0; gx < frame->grid_width; ++gx) {
        int sx = min(image.width - 1, gx * PIXEL_STEP);
        int sy = min(image.height - 1, gy * PIXEL_STEP);
        const unsigned char *source = image.bgra + ((size_t)sy * image.width + sx) * 4;
        unsigned char *output = frame->bgr +
            ((size_t)gy * frame->grid_width + gx) * 3;
        memcpy(output, source, 3);
    }
    bongo_cat_validation_image_free(&image);
    return 1;
}

void phase_free_frames(Frame *frames, size_t count) {
    size_t index;
    for (index = 0; index < count; ++index) free(frames[index].bgr);
    free(frames);
}

/* 一次性把所有帧读进内存, 并要求所有帧尺寸一致(不一致说明这一组根本不是同一个
   模型的抽帧, 直接整体失败), 这样后面比较时就不必逐帧判尺寸。 */
Frame *phase_load_frames(const PathList *paths) {
    Frame *frames = (Frame *)calloc(paths->count, sizeof(*frames));
    size_t index;
    if (!frames) return NULL;
    for (index = 0; index < paths->count; ++index) {
        if (!load_frame(paths->items[index], &frames[index]) || (index &&
            (frames[index].width != frames[0].width ||
                frames[index].height != frames[0].height))) {
            phase_free_frames(frames, paths->count); return NULL;
        }
    }
    return frames;
}
