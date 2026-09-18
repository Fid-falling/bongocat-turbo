#include "preferences_overlay.h"
#include "ui_animation.h"
#include "ui_paint.h"

#include <SDL3/SDL.h>

enum { OVERLAY_OPEN_MS = 200, OVERLAY_CLOSE_MS = 180 };

bool bongo_cat_preferences_overlay_input_ready(struct nk_context *context,
    bool *armed) {
    if (!context || !armed) return false;
    if (*armed) return true;
    const struct nk_mouse_button *left =
        &context->input.mouse.buttons[NK_BUTTON_LEFT];
    if (!left->down) *armed = true;
    return false;
}

struct nk_color bongo_cat_preferences_overlay_alpha(
    struct nk_color color, float visibility) {
    color.a = (nk_byte)(color.a * NK_CLAMP(0.0f, visibility, 1.0f) + .5f);
    return color;
}

BongoCatOverlayFrame bongo_cat_preferences_overlay_frame(
    struct nk_rect region, float width, float height, uint64_t opened_ns,
    uint64_t closing_ns) {
    uint64_t now = SDL_GetTicksNS();
    float visibility = 1.0f, close = 0.0f;
    bool finished = false;
    if (closing_ns) {
        float progress = (float)(now - closing_ns) /
            (OVERLAY_CLOSE_MS * 1000000.0f);
        finished = progress >= 1.0f;
        close = bongo_cat_ui_ease(BONGO_CAT_UI_EASE_STANDARD,
            NK_CLAMP(0.0f, progress, 1.0f));
        visibility = 1.0f - close;
    } else if (opened_ns) {
        float progress = (float)(now - opened_ns) /
            (OVERLAY_OPEN_MS * 1000000.0f);
        visibility = bongo_cat_ui_ease(BONGO_CAT_UI_EASE_STANDARD,
            NK_CLAMP(0.0f, progress, 1.0f));
    }
    float scale = 1.0f - .015f * close;
    float panel_width = width * scale, panel_height = height * scale;
    BongoCatOverlayFrame frame = {
        .panel = nk_rect(region.x + (region.w - panel_width) * .5f,
            region.y + (region.h - panel_height) * .5f + 6.0f * close,
            panel_width, panel_height),
        .visibility = visibility, .close_amount = close, .finished = finished};
    return frame;
}

void bongo_cat_preferences_overlay_draw(struct nk_context *context,
    struct nk_rect region, const BongoCatOverlayFrame *frame,
    BongoCatUIPalette palette) {
    struct nk_command_buffer *canvas = nk_window_get_canvas(context);
    bool dark = bongo_cat_ui_dark(context);
    struct nk_color veil = dark ? nk_rgba(7, 10, 15,
        (nk_byte)(92.0f * frame->visibility + .5f)) : nk_rgba(255, 255, 255,
        (nk_byte)(148.0f * frame->visibility + .5f));
    nk_push_scissor(canvas, region);
    nk_fill_rect(canvas, region, 24, veil);
    if (palette.effects) bongo_cat_ui_paint_shadow(context, frame->panel,
        18, 0, 20, 50, 0, nk_rgba(0, 0, 0,
        (nk_byte)(77.0f * frame->visibility + .5f)));
}
