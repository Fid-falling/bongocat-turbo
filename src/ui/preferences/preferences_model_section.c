#include "preferences_model_card.h"
#include "preferences_state.h"
#include "ui_animation.h"
#include "ui_backend.h"
#include "ui_catime.h"
#include "ui_paint.h"
#include "bongo_cat/i18n.h"

static const char *tr(BongoCatPreferences *value, const char *key,
    const char *fallback) {
    return bongo_cat_i18n_get(value->app->i18n, key, fallback);
}

bool bongo_cat_preferences_model_section(BongoCatPreferences *value,
    struct nk_context *context) {
    struct nk_rect bounds;
    nk_layout_row_dynamic(context, 22, 1);
    if (nk_widget(&bounds, context) == NK_WIDGET_INVALID) return false;
    BongoCatUIPalette p = bongo_cat_ui_palette(bongo_cat_ui_dark(context));
    struct nk_command_buffer *canvas = nk_window_get_canvas(context);
    const struct nk_user_font *action_font = bongo_cat_ui_caption_font(context);
    const char *label = tr(value,
        "pages.preference.model.tooltips.moreModels", "More Models");
    float label_width = action_font->width(action_font->userdata,
        action_font->height, label, nk_strlen(label));
    float action_width = label_width + 22;
    float action_x = bounds.x + bounds.w - action_width;
    struct nk_rect action = nk_rect(action_x,
        bounds.y, action_width, bounds.h);
    bool action_hover = nk_input_is_mouse_hovering_rect(&context->input, action);
    float hover_amount = bongo_cat_ui_animate_eased(context,
        "model-more-link-hover", action_hover ? 1.0f : 0.0f, 180.0f,
        BONGO_CAT_UI_EASE_STANDARD);
    struct nk_color action_color = p.danger;
    nk_draw_text(canvas, nk_rect(action.x, action.y +
        (action.h - action_font->height) * .5f, label_width + 1,
        action_font->height), label, nk_strlen(label), action_font,
        nk_rgba(0, 0, 0, 0), action_color);
    float arrow_y = action.y + action.h * .5f;
    float arrow_offset = 2.0f * hover_amount;
    float first_arrow_x = action.x + label_width + 11 + arrow_offset;
    float second_arrow_x = action.x + label_width + 17 + arrow_offset;
    struct nk_color first_arrow_color = bongo_cat_ui_color_alpha(
        action_color, 0.5f);
    nk_stroke_line(canvas, first_arrow_x - 4, arrow_y - 4,
        first_arrow_x, arrow_y, 1.5f, first_arrow_color);
    nk_stroke_line(canvas, first_arrow_x, arrow_y,
        first_arrow_x - 4, arrow_y + 4, 1.5f, first_arrow_color);
    nk_stroke_line(canvas, second_arrow_x - 4, arrow_y - 4,
        second_arrow_x, arrow_y, 1.5f, action_color);
    nk_stroke_line(canvas, second_arrow_x, arrow_y,
        second_arrow_x - 4, arrow_y + 4, 1.5f, action_color);
    if (action_hover) bongo_cat_ui_cursor_hover_rect(context, action,
        BONGO_CAT_UI_CURSOR_POINTER);
    return action_hover && nk_input_is_mouse_click_in_rect(&context->input,
        NK_BUTTON_LEFT, action);
}
