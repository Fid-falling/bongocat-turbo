#ifndef BONGO_CAT_MODAL_FRAME_INTERNAL_H
#define BONGO_CAT_MODAL_FRAME_INTERNAL_H

#include "runtime.h"

typedef struct BongoCatModalFrame {
    BongoCatApp *app;
    uint64_t last_tick_ns;
    uint64_t tick_count;
} BongoCatModalFrame;

void bongo_cat_modal_frame_init(BongoCatModalFrame *state, BongoCatApp *app);
void bongo_cat_modal_frame_tick(void *userdata);

#endif
