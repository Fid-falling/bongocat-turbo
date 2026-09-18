#ifndef BONGO_CAT_PREFERENCES_SCROLLBAR_H
#define BONGO_CAT_PREFERENCES_SCROLLBAR_H

#include "nuklear_config.h"

#include <stdbool.h>

typedef struct BongoCatPreferencesScrollbar {
    float grab_y;
    bool dragging;
} BongoCatPreferencesScrollbar;

typedef struct BongoCatPreferencesScrollbarResult {
    float offset;
    bool scrollable;
    bool changed;
} BongoCatPreferencesScrollbarResult;

bool bongo_cat_preferences_scrollbar_needed(
    struct nk_rect viewport, float content_height);
float bongo_cat_preferences_scrollbar_content_width(
    struct nk_rect viewport, float content_height);
void bongo_cat_preferences_scrollbar_reset(
    BongoCatPreferencesScrollbar *scrollbar);
BongoCatPreferencesScrollbarResult bongo_cat_preferences_scrollbar_draw(
    struct nk_context *context, struct nk_command_buffer *canvas,
    struct nk_rect viewport, float content_height, float offset,
    BongoCatPreferencesScrollbar *scrollbar, struct nk_color cursor_color,
    float opacity, bool enabled);

#endif
