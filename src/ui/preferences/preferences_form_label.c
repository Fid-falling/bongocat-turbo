#include "preferences_widgets.h"
#include "preferences_widgets_internal.h"
#include "ui_backend.h"
#include "ui_catime.h"

static struct nk_context *icon_context;
static BongoCatPrefIcon pending_icon;
static bool icon_pending;

void bongo_cat_pref_row_icon(struct nk_context *context,
    BongoCatPrefIcon icon) {
    icon_context = context;
    pending_icon = icon;
    icon_pending = true;
}

void bongo_cat_pref_row_icon_clear(struct nk_context *context) {
    if (icon_context != context) return;
    icon_context = NULL;
    icon_pending = false;
}

void bongo_cat_pref_form_title_sized(struct nk_context *context,
    const char *title, float control_width) {
    float available = nk_window_get_content_region(context).w;
    float left = NK_MAX(220.0f, available - control_width - 8.0f);
    nk_layout_row_begin(context, NK_STATIC, 36, 2);
    nk_layout_row_push(context, left);
    bongo_cat_pref_form_label(context, title);
    nk_layout_row_push(context, NK_MAX(control_width,
        available - left - 8.0f));
}

void bongo_cat_pref_form_label(struct nk_context *context, const char *title) {
    if (!icon_pending || icon_context != context) {
        nk_style_push_font(context, bongo_cat_ui_label_font(context));
        struct nk_vec2 padding = context->style.text.padding;
        context->style.text.padding.x += 5.0f;
        nk_label(context, title, NK_TEXT_LEFT);
        context->style.text.padding = padding;
        nk_style_pop_font(context);
        return;
    }
    struct nk_rect bounds;
    enum nk_widget_layout_states state = nk_widget(&bounds, context);
    BongoCatPrefIcon icon = pending_icon;
    bongo_cat_pref_row_icon_clear(context);
    if (state == NK_WIDGET_INVALID) return;
    BongoCatUIPalette p = bongo_cat_ui_palette(bongo_cat_ui_dark(context));
    struct nk_command_buffer *canvas = nk_window_get_canvas(context);
    struct nk_rect icon_bounds = nk_rect(bounds.x + 5,
        bounds.y + (bounds.h - 18) * .5f, 18, 18);
    bongo_cat_pref_icon_draw(canvas, icon_bounds, icon, p.accent);
    const struct nk_user_font *font = bongo_cat_ui_label_font(context);
    struct nk_rect text = nk_rect(bounds.x + 33,
        bounds.y + (bounds.h - font->height) * .5f,
        bounds.w - 33, font->height);
    nk_draw_text(canvas, text, title, nk_strlen(title), font,
        nk_rgba(0, 0, 0, 0), p.text);
}
