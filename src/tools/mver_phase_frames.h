#ifndef BONGO_CAT_MVER_PHASE_FRAMES_H
#define BONGO_CAT_MVER_PHASE_FRAMES_H

#include <stddef.h>
#include <wchar.h>

/*
 * Mver 相位验证工具共用的帧加载: 把目录里的 PNG/BMP 读成一串等尺寸的帧。
 *
 * 抽样步长固定为 4 像素: 相位比较只关心大块底色和眼睛/嘴巴的位置, 全分辨率
 * 比对既慢又容易被抗锯齿噪点干扰。
 */
enum { PIXEL_STEP = 4 };

typedef struct PathList {
    wchar_t **items;
    size_t count;
    size_t capacity;
} PathList;

typedef struct Frame {
    int width, height, grid_width, grid_height;
    unsigned char *bgr;
} Frame;

int phase_collect_paths(const wchar_t *directory, PathList *list);
void phase_free_paths(PathList *list);
Frame *phase_load_frames(const PathList *paths);
void phase_free_frames(Frame *frames, size_t count);

#endif
