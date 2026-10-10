/* 模态循环的手动帧推进器 (实现与 dt 规则见 modal_frame.c): 结构体由调用方按值持有, app 是
 * 借用指针; init 之后每帧调一次 tick, last_tick_ns/tick_count 由实现维护, 调用方不要直接改。 */
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
