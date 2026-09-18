#ifndef BONGO_CAT_PREFERENCES_MODEL_CARD_PAINT_H
#define BONGO_CAT_PREFERENCES_MODEL_CARD_PAINT_H

#include "preferences_state.h"

struct nk_rect bongo_cat_preferences_model_card_outline_bounds(
    struct nk_rect bounds, float thickness);
void bongo_cat_preferences_model_card_draw_progress(
    struct nk_command_buffer *canvas, struct nk_rect bounds, float rounding,
    float thickness, float progress, struct nk_color color);
void bongo_cat_preferences_model_card_draw_selection_badge(
    BongoCatPreferences *value, struct nk_command_buffer *canvas,
    struct nk_rect preview, BongoCatUIPalette palette, float amount);

#endif
