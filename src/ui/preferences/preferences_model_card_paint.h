#ifndef BONGO_CAT_PREFERENCES_MODEL_CARD_PAINT_H
#define BONGO_CAT_PREFERENCES_MODEL_CARD_PAINT_H

#include "preferences_state.h"

/*
 * 模型卡片上的自绘装饰 (实现在 preferences_model_card_paint.c)。
 * 三个函数的 canvas 都由调用方给 (整页共用一个命令缓冲), 颜色也由调用方给 ——
 * 这样它们不必知道主题是怎么解析出来的。
 * amount=0 / progress<=0 时函数直接返回不画, 调用方不必自己判断。
 */
struct nk_rect bongo_cat_preferences_model_card_outline_bounds(
    struct nk_rect bounds, float thickness);
void bongo_cat_preferences_model_card_draw_progress(
    struct nk_command_buffer *canvas, struct nk_rect bounds, float rounding,
    float thickness, float progress, struct nk_color color);
void bongo_cat_preferences_model_card_draw_selection_badge(
    BongoCatPreferences *value, struct nk_command_buffer *canvas,
    struct nk_rect preview, BongoCatUIPalette palette, float amount);
void bongo_cat_preferences_model_card_draw_corner_marker(
    struct nk_command_buffer *canvas, float x, float y, float size,
    float radius, struct nk_color color);

#endif