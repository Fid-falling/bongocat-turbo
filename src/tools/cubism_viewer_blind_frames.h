#ifndef BONGO_CAT_CUBISM_VIEWER_BLIND_FRAMES_H
#define BONGO_CAT_CUBISM_VIEWER_BLIND_FRAMES_H

#include "validation_image.h"

#include <stddef.h>
#include <wchar.h>

/*
 * "盲测"工具共用的帧收集: 一个 Frame 同时装着同一场景在 viewer 和原生实现下的
 * 两张渲染结果, 供后面的比对工具逐对比较。
 *
 * 配对靠文件名: 两个目录里同名的图片配成一对(见 blind_collect_frames), 所以
 * 采集脚本必须保证两侧命名一致。
 */
typedef struct Frame {
    wchar_t *name;
    wchar_t *viewer_path;
    wchar_t *native_path;
    BongoCatValidationImage viewer;
    BongoCatValidationImage native;
} Frame;

typedef struct FrameList {
    Frame *items;
    size_t count;
    size_t capacity;
} FrameList;

/* 下面两个拼接函数返回新分配的字符串, 由调用方负责 free; 失败返回 NULL。 */
wchar_t *blind_join_path(const wchar_t *directory, const wchar_t *name);
wchar_t *blind_join_suffix(const wchar_t *directory, const wchar_t *name,
    const wchar_t *suffix);
void blind_free_frames(FrameList *list);
int blind_collect_frames(const wchar_t *viewer_directory,
    const wchar_t *native_directory, FrameList *list);

#endif
