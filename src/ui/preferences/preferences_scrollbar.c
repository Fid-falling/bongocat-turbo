#include "preferences_scrollbar.h"

#include "ui_backend.h"

enum { SCROLLBAR_WIDTH = 10, SCROLLBAR_RESERVE = 14 };
static const float SCROLLBAR_WHEEL_STEP = 48.0f;

static struct nk_color with_opacity(struct nk_color color, float opacity) {
    color.a = (nk_byte)(color.a * NK_CLAMP(0.0f, opacity, 1.0f) + .5f);
    return color;
}

static void draw_track(struct nk_command_buffer *canvas, struct nk_rect bounds,
    const struct nk_style_item *item, const struct nk_style_scrollbar *style,
    float opacity) {
    struct nk_color tint = with_opacity(nk_rgba(255, 255, 255, 255), opacity);
    switch (item->type) {
    case NK_STYLE_ITEM_IMAGE:
        nk_draw_image(canvas, bounds, &item->data.image, tint);
        break;
    case NK_STYLE_ITEM_NINE_SLICE:
        nk_draw_nine_slice(canvas, bounds, &item->data.slice, tint);
        break;
    case NK_STYLE_ITEM_COLOR:
        nk_fill_rect(canvas, bounds, style->rounding,
            with_opacity(item->data.color, opacity));
        if (style->border > 0.0f)
            nk_stroke_rect(canvas, bounds, style->rounding, style->border,
                with_opacity(style->border_color, opacity));
        break;
    }
}

bool bongo_cat_preferences_scrollbar_needed(
    struct nk_rect viewport, float content_height) {
    return content_height > viewport.h;
}

float bongo_cat_preferences_scrollbar_content_width(
    struct nk_rect viewport, float content_height) {
    return viewport.w - (bongo_cat_preferences_scrollbar_needed(
        viewport, content_height) ? SCROLLBAR_RESERVE : 0.0f);
}

void bongo_cat_preferences_scrollbar_reset(
    BongoCatPreferencesScrollbar *scrollbar) {
    if (!scrollbar) return;
    scrollbar->grab_y = 0.0f;
    scrollbar->dragging = false;
}

BongoCatPreferencesScrollbarResult bongo_cat_preferences_scrollbar_draw(
    struct nk_context *context, struct nk_command_buffer *canvas,
    struct nk_rect viewport, float content_height, float offset,
    BongoCatPreferencesScrollbar *scrollbar, struct nk_color cursor_color,
    float opacity, bool enabled) {
    BongoCatPreferencesScrollbarResult result = {
        .offset = offset,
        .scrollable = bongo_cat_preferences_scrollbar_needed(
            viewport, content_height),
        .changed = false};
    if (!context || !canvas || !scrollbar) return result;
    if (!result.scrollable) {
        bongo_cat_preferences_scrollbar_reset(scrollbar);
        result.changed = offset != 0.0f;
        result.offset = 0.0f;
        return result;
    }

    const struct nk_style_scrollbar *style = &context->style.scrollv;
    struct nk_rect track = nk_rect(viewport.x + viewport.w - SCROLLBAR_WIDTH,
        viewport.y, SCROLLBAR_WIDTH, viewport.h);
    float initial = offset;
    float maximum = content_height - viewport.h;
    offset = NK_CLAMP(0.0f, offset, maximum);
    bool hover = enabled && nk_input_is_mouse_hovering_rect(
        &context->input, track);
    if (!enabled) scrollbar->dragging = false;
    float padding_x = hover || scrollbar->dragging ? 0.0f : style->padding.x;
    float inset_y = style->border + style->padding.y;
    float cursor_height = viewport.h / content_height * viewport.h -
        inset_y * 2.0f;
    struct nk_rect cursor = nk_rect(track.x + style->border + padding_x,
        track.y + offset / content_height * track.h + inset_y,
        track.w - (style->border + padding_x) * 2.0f,
        NK_MAX(0.0f, cursor_height));

    struct nk_mouse_button *left = &context->input.mouse.buttons[NK_BUTTON_LEFT];
    if (!left->down) scrollbar->dragging = false;
    if (enabled && left->clicked && NK_INBOX(left->clicked_pos.x,
        left->clicked_pos.y, cursor.x, cursor.y, cursor.w, cursor.h)) {
        scrollbar->dragging = true;
        scrollbar->grab_y = left->clicked_pos.y - cursor.y;
    } else if (enabled && left->clicked && hover) {
        offset += left->clicked_pos.y < cursor.y ? -viewport.h : viewport.h;
        offset = NK_CLAMP(0.0f, offset, maximum);
    }
    if (enabled && scrollbar->dragging && left->down) {
        float travel = NK_MAX(1.0f, track.h - inset_y * 2.0f - cursor.h);
        float cursor_y = context->input.mouse.pos.y - track.y - inset_y -
            scrollbar->grab_y;
        offset = NK_CLAMP(0.0f, cursor_y / travel * maximum, maximum);
    }
    bool viewport_hover = enabled && nk_input_is_mouse_hovering_rect(
        &context->input, viewport);
    if (viewport_hover && context->input.mouse.scroll_delta.y != 0.0f)
        offset = NK_CLAMP(0.0f, offset - context->input.mouse.scroll_delta.y *
            SCROLLBAR_WHEEL_STEP, maximum);

    bool active = hover || scrollbar->dragging;
    const struct nk_style_item *track_style = scrollbar->dragging ?
        &style->active : hover ? &style->hover : &style->normal;
    draw_track(canvas, track, track_style, style, opacity);
    cursor.x = track.x + style->border +
        (active ? 0.0f : style->padding.x);
    cursor.w = track.w - (cursor.x - track.x) * 2.0f;
    cursor.y = track.y + offset / content_height * track.h + inset_y;
    nk_fill_rect(canvas, cursor, style->rounding_cursor,
        with_opacity(cursor_color, opacity));
    if (style->border_cursor > 0.0f)
        nk_stroke_rect(canvas, cursor, style->rounding_cursor,
            style->border_cursor,
            with_opacity(style->cursor_border_color, opacity));
    if (hover) bongo_cat_ui_cursor_hover_rect(context, track,
        BONGO_CAT_UI_CURSOR_POINTER);

    result.offset = offset;
    result.changed = offset != initial;
    return result;
}
