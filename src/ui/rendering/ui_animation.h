#ifndef BONGO_CAT_UI_ANIMATION_H
#define BONGO_CAT_UI_ANIMATION_H

#include "nuklear_config.h"
#include <stdbool.h>

typedef enum BongoCatUIEasing {
    BONGO_CAT_UI_EASE_LINEAR,
    BONGO_CAT_UI_EASE_OUT_CUBIC,
    BONGO_CAT_UI_EASE_STANDARD,
    BONGO_CAT_UI_EASE_SWIFT,
    BONGO_CAT_UI_EASE_SPRING
} BongoCatUIEasing;

float bongo_cat_ui_ease(BongoCatUIEasing easing, float progress);
float bongo_cat_ui_animate_eased(struct nk_context *context,
    const char *id, float target, float duration_ms, BongoCatUIEasing easing);
float bongo_cat_ui_animate(struct nk_context *context, const char *id,
    float target, float duration_ms);
float bongo_cat_ui_animate_pulse(struct nk_context *context,
    const char *id, bool triggered, float hold_ms, float fade_ms);
bool bongo_cat_ui_animations_active(const struct nk_context *context);
void bongo_cat_ui_animations_reset(const struct nk_context *context);

#endif
