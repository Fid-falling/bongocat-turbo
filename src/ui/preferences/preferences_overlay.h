#ifndef BONGO_CAT_PREFERENCES_OVERLAY_H
#define BONGO_CAT_PREFERENCES_OVERLAY_H

#include "ui_catime.h"

#include <stdint.h>

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
