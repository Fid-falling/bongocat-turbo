#ifndef BONGO_CAT_MVER_PHASE_FRAMES_H
#define BONGO_CAT_MVER_PHASE_FRAMES_H

#include <stddef.h>
#include <wchar.h>

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
