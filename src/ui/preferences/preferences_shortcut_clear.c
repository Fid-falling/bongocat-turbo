/*
 * 快捷键框右边那个小叉 (清除绑定)。
 *
 * 单独成一个文件是因为它同时被两处用: 行为设置对话框里的行 (preferences_behavior_row.c)
 * 和快捷键页的行 (preferences_widget_shortcut.c)。两边样式要一致, 所以绘制与命中都
 * 收在这里。
 * enabled 为假时**直接返回 false** 且不画任何东西: 没有绑定可清的行不该出现一个小叉,
 * 更需要保证"清除"不会误触发到别的命中区上。
 * 动画 id 带调用方的 id 前缀, 多个清除按钮的悬停才不会联动。
 */
#include "preferences_shortcut_clear.h"
#include "ui_animation.h"
#include "ui_backend.h"
#include "ui_paint.h"

#include <stdio.h>

bool bongo_cat_pref_shortcut_clear(struct nk_context *context,
    struct nk_command_buffer *canvas, const char *id, struct nk_rect bounds,
    BongoCatUIPalette p, float opacity, bool enabled) {
    if (!enabled) return false;
    bool hover = nk_input_is_mouse_hovering_rect(&context->input, bounds);
    char animation_id[128];
    snprintf(animation_id, sizeof(animation_id), "shortcut-clear-%s", id);
    float amount = bongo_cat_ui_animate_eased(context, animation_id,
        hover ? 1.0f : 0.0f, 150, BONGO_CAT_UI_EASE_STANDARD);
    struct nk_color color = bongo_cat_ui_color_alpha(
        bongo_cat_ui_color_mix(p.muted, p.accent, amount), opacity);
    float x = bounds.x + bounds.w * .5f, y = bounds.y + bounds.h * .5f;
    nk_stroke_line(canvas, x - 4, y - 4, x + 4, y + 4, 1.5f, color);
    nk_stroke_line(canvas, x + 4, y - 4, x - 4, y + 4, 1.5f, color);
    if (hover) bongo_cat_ui_cursor_hover_rect(context, bounds,
        BONGO_CAT_UI_CURSOR_POINTER);
    return hover && nk_input_is_mouse_click_in_rect(&context->input,
        NK_BUTTON_LEFT, bounds);
}
