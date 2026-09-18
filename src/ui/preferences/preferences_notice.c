#include "preferences_notice.h"
#include "preferences_state.h"
#include "ui_animation.h"
#include "ui_backend.h"
#include "ui_catime.h"
#include "ui_paint.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

enum { NOTICE_DURATION_MS = 2600, NOTICE_ENTER_MS = 250 };

void bongo_cat_preferences_notice_show(BongoCatApp *app,
    const char *message, bool error) {
    if (!app || !app->preferences || !message || !message[0]) return;
    BongoCatPreferences *value = app->preferences;
    uint64_t now = SDL_GetTicksNS();
    BongoCatPreferenceNotice *target = &value->notices[0];
    for (size_t i = 0; i < sizeof(value->notices) / sizeof(value->notices[0]); ++i) {
        BongoCatPreferenceNotice *notice = &value->notices[i];
        if (!notice->message[0] || notice->until_ns <= now) {
            target = notice; break;
        }
        if (notice->started_ns < target->started_ns) target = notice;
    }
    snprintf(target->message, sizeof(target->message), "%s", message);
    target->error = error;
    target->started_ns = now;
    target->until_ns = now + NOTICE_DURATION_MS * 1000000ULL;
    value->render_dirty = true;
}

static size_t active_notices(BongoCatPreferences *value, uint64_t now,
    BongoCatPreferenceNotice **items) {
    size_t count = 0;
    for (size_t i = 0; i < sizeof(value->notices) / sizeof(value->notices[0]); ++i) {
        BongoCatPreferenceNotice *notice = &value->notices[i];
        if (notice->message[0] && notice->until_ns <= now)
            memset(notice, 0, sizeof(*notice));
        if (notice->message[0]) items[count++] = notice;
    }
    for (size_t i = 1; i < count; ++i) {
        BongoCatPreferenceNotice *item = items[i];
        size_t j = i;
        while (j && items[j - 1]->started_ns > item->started_ns) {
            items[j] = items[j - 1]; j--;
        }
        items[j] = item;
    }
    return count;
}

static float fit_notice_text(const struct nk_user_font *font,
    const char *source, float maximum, char *output, size_t capacity) {
    snprintf(output, capacity, "%s", source);
    float width = font->width(font->userdata, font->height,
        output, nk_strlen(output));
    if (width <= maximum) return width;
    size_t prefix = strlen(source);
    if (prefix > capacity - 4) prefix = capacity - 4;
    while (prefix) {
        prefix--;
        while (prefix && ((unsigned char)source[prefix] & 0xc0) == 0x80)
            prefix--;
        memcpy(output, source, prefix);
        memcpy(output + prefix, "...", 4);
        width = font->width(font->userdata, font->height,
            output, nk_strlen(output));
        if (width <= maximum) return width;
    }
    snprintf(output, capacity, "...");
    return font->width(font->userdata, font->height, output, 3);
}

static void draw_notice(BongoCatPreferences *value,
    struct nk_context *context, BongoCatPreferenceNotice *notice,
    float width, float y, uint64_t now) {
    BongoCatUIPalette p = bongo_cat_ui_palette(
        bongo_cat_ui_dark(context));
    const struct nk_user_font *font = bongo_cat_ui_label_font(context);
    char label_text[sizeof(notice->message)];
    float text_width = fit_notice_text(font, notice->message,
        NK_MAX(40.0f, width - 100.0f), label_text, sizeof(label_text));
    float toast_width = NK_MIN(text_width + 36.0f, width - 64.0f);
    float elapsed = (float)(now - notice->started_ns) /
        (NOTICE_ENTER_MS * 1000000.0f);
    float progress = bongo_cat_ui_ease(BONGO_CAT_UI_EASE_SPRING,
        NK_CLAMP(0.0f, elapsed, 1.0f));
    float opacity = NK_CLAMP(0.0f, progress, 1.0f);
    float shown_width = toast_width * (.9f + .1f * progress);
    struct nk_rect bounds = nk_rect((width - shown_width) * .5f,
        y - 12.0f * (1.0f - progress), shown_width, 41.0f);
    struct nk_color tone = notice->error ? p.danger : p.pink;
    if (p.effects) bongo_cat_ui_paint_shadow(context, bounds, 20, 0, 8,
        notice->error ? 25.0f : 22.0f, 0, nk_rgba(tone.r, tone.g, tone.b,
        (nk_byte)((notice->error ? 102 : 89) * opacity)));
    struct nk_command_buffer *canvas = nk_window_get_canvas(context);
    nk_fill_rect(canvas, bounds, 20, nk_rgba(tone.r, tone.g, tone.b,
        (nk_byte)(255 * opacity)));
    struct nk_rect label = nk_rect(bounds.x + (bounds.w - text_width) * .5f,
        bounds.y + (bounds.h - font->height) * .5f,
        NK_MIN(text_width + 1, bounds.w - 20), font->height);
    nk_draw_text(canvas, label, label_text, nk_strlen(label_text), font,
        nk_rgba(0, 0, 0, 0), nk_rgba(255, 255, 255, (nk_byte)(255 * opacity)));
    value->render_dirty = true;
}

void bongo_cat_preferences_notice_draw(BongoCatPreferences *value,
    struct nk_context *context, float width, float height) {
    (void)height;
    if (!value) return;
    uint64_t now = SDL_GetTicksNS();
    BongoCatPreferenceNotice *items[4];
    size_t count = active_notices(value, now, items);
    for (size_t i = 0; i < count; ++i)
        draw_notice(value, context, items[i], width, 20.0f + i * 51.0f, now);
}
