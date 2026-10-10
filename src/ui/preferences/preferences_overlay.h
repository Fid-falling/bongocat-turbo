#ifndef BONGO_CAT_PREFERENCES_OVERLAY_H
#define BONGO_CAT_PREFERENCES_OVERLAY_H

#include "ui_catime.h"

#include <stdint.h>

/*
 * 模态弹层的公共构件。
 * frame() 只算几何与动画 (不画任何东西), draw() 只画遮罩与阴影; 面板内容由调用方
 * 自己画 —— 这样删除确认框和行为设置对话框可以共用同一套开关动画与遮罩。
 * input_ready 需要调用方提供一个持久的 bool (通常存在自己的弹层状态里), 它是
 * "吃掉打开弹层那一次点击"的状态位。
 */
typedef struct BongoCatOverlayFrame {
    struct nk_rect panel;
    float visibility;
    float close_amount;
    bool finished;
} BongoCatOverlayFrame;

BongoCatOverlayFrame bongo_cat_preferences_overlay_frame(
    struct nk_rect region, float width, float height, uint64_t opened_ns,
    uint64_t closing_ns);
bool bongo_cat_preferences_overlay_input_ready(struct nk_context *context,
    bool *armed);
void bongo_cat_preferences_overlay_draw(struct nk_context *context,
    struct nk_rect region, const BongoCatOverlayFrame *frame,
    BongoCatUIPalette palette);
struct nk_color bongo_cat_preferences_overlay_alpha(
    struct nk_color color, float visibility);

#endif
