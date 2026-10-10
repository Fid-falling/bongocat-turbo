/* 参考 trace 的帧号反查 (只有 diagnostics 用): track/returning 各 6 个 int 由调用方提供, 顺序必须与
 * live2d_viewer_audit.c 里的 track_names/return_names 一致; 没给路径视为通过, 数据不完整视为失败。
 */
#ifndef BONGO_CAT_LIVE2D_VIEWER_TIMING_H
#define BONGO_CAT_LIVE2D_VIEWER_TIMING_H

#include <stdbool.h>

bool bongo_cat_live2d_viewer_timing(const char *path, int *track,
    int *returning);

#endif
