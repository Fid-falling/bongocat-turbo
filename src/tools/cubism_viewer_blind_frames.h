#ifndef BONGO_CAT_CUBISM_VIEWER_BLIND_FRAMES_H
#define BONGO_CAT_CUBISM_VIEWER_BLIND_FRAMES_H

#include "validation_image.h"

#include <stddef.h>
#include <wchar.h>

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

wchar_t *blind_join_path(const wchar_t *directory, const wchar_t *name);
wchar_t *blind_join_suffix(const wchar_t *directory, const wchar_t *name,
    const wchar_t *suffix);
void blind_free_frames(FrameList *list);
int blind_collect_frames(const wchar_t *viewer_directory,
    const wchar_t *native_directory, FrameList *list);

#endif
