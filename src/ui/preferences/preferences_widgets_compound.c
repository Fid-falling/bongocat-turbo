/*
 * 复合控件: "开关 + 可选数值输入"排在一行 (例如"随机表情" + 间隔秒数)。
 * 关键点是列数随开关状态变: 关掉时是 2 列 (标题 + 开关), 打开时是 3 列
 * (标题 + 数值 + 开关) —— 数值框不是"禁用"而是整个不占位, 这样关掉时开关会靠到
 * 右边, 视觉上不会有空洞。
 * 两处 changed 用 || 合并: 用户可能在同一帧里既拖了数值又点了开关, 漏一个就会丢改动。
 */
#include <stdio.h>
#include "preferences_widgets.h"
#include "preferences_widgets_internal.h"
#include "preferences_controls.h"
#include "ui_backend.h"
#include "ui_catime.h"
#include "ui_icons.h"

bool bongo_cat_pref_toggle_float(struct nk_context *context, const char *id,
    const char *title, bool *enabled, float minimum, float *value,
    float maximum, float step, float default_value) {
    FormStyle saved;
    if (!bongo_cat_pref_form_begin(context, id, 0, &saved)) return false;
    float available = nk_window_get_content_region(context).w;
    int columns = *enabled ? 3 : 2;
    float input_width = *enabled ? 122.0f : 0.0f;
    float spacing = 8.0f * (columns - 1);
    float left = NK_MAX(220.0f,
        available - input_width - 80.0f - spacing);
    nk_layout_row_begin(context, NK_STATIC, 36, columns);
    nk_layout_row_push(context, left);
    bongo_cat_pref_form_label(context, title);
    bool changed = false;
    if (*enabled) {
        nk_layout_row_push(context, input_width);
        changed = bongo_cat_pref_control_float(context, id,
            minimum, value, maximum, step, default_value);
    }
    nk_layout_row_push(context, NK_MAX(80.0f,
        available - left - input_width - spacing));
    changed = bongo_cat_pref_control_toggle(context, id, enabled) || changed;
    nk_layout_row_end(context);
    bongo_cat_pref_form_end(context, &saved);
    return changed;
}

/* "开关 + 可选滑杆"排在一行 (例如"边缘吸附" + 吸附阈值): 布局与 toggle_float 相同,
   唯一区别是左侧数值控件画成滑杆而不是步进输入框, 需要更宽的中间列。开启时数值列
   用 suffix 决定的单位 (百分比 "%" 或像素 " px"); 标题下方还有 description 提示行。 */
bool bongo_cat_pref_toggle_slider(struct nk_context *context, const char *id,
    const char *title, const char *description, bool *enabled,
    float minimum, float *value, float maximum, float step,
    float default_value, const char *suffix) {
    int lines = bongo_cat_pref_detail_lines(context, description);
    FormStyle saved;
    if (!bongo_cat_pref_form_begin(context, id, lines, &saved)) return false;
    float available = nk_window_get_content_region(context).w;
    int columns = *enabled ? 3 : 2;
    float input_width = *enabled ? 260.0f : 0.0f;
    float spacing = 8.0f * (columns - 1);
    float left = NK_MAX(220.0f,
        available - input_width - 80.0f - spacing);
    nk_layout_row_begin(context, NK_STATIC, 36, columns);
    nk_layout_row_push(context, left);
    bongo_cat_pref_form_label(context, title);
    bool changed = false;
    if (*enabled) {
        nk_layout_row_push(context, input_width);
        changed = bongo_cat_pref_control_slider(context, id,
            minimum, value, maximum, step, default_value, suffix);
    }
    nk_layout_row_push(context, NK_MAX(80.0f,
        available - left - input_width - spacing));
    changed = bongo_cat_pref_control_toggle(context, id, enabled) || changed;
    nk_layout_row_end(context);
    bongo_cat_pref_description(context, description, lines);
    bongo_cat_pref_form_end(context, &saved);
    return changed;
}

/* "开关 + 可选数值输入 + 设置按钮", 用来对齐 X 的随机表情行: 标题右侧带单位括号
   "(s)", 开启时多一列 36px 的设置按钮 (齿轮), 标题下方还有 detail 提示行。
   返回 bool 是"值变了"; 是否点了设置按钮通过 config_clicked 带出 —— 调用方据此
   打开参与列表对话框。布局同 toggle_float, 只是开启时从 3 列变 4 列。 */
static bool bongo_cat_pref_config_button(struct nk_context *context) {
    struct nk_rect bounds;
    if (nk_widget(&bounds, context) == NK_WIDGET_INVALID) return false;
    BongoCatUIPalette p = bongo_cat_ui_palette(bongo_cat_ui_dark(context));
    struct nk_command_buffer *canvas = nk_window_get_canvas(context);
    bool hover = nk_input_is_mouse_hovering_rect(&context->input, bounds);
    nk_fill_rect(canvas, bounds, 10, hover ? p.hover : p.field);
    nk_stroke_rect(canvas, bounds, 10, 1, hover ? p.accent : p.border_subtle);
    struct nk_rect icon = nk_rect(bounds.x + (bounds.w - 16.0f) * .5f,
        bounds.y + (bounds.h - 16.0f) * .5f, 16.0f, 16.0f);
    bongo_cat_ui_draw_icon(canvas, BONGO_CAT_UI_ICON_SETTINGS, icon,
        hover ? p.accent : p.muted);
    if (hover) bongo_cat_ui_cursor_hover_rect(context, bounds,
        BONGO_CAT_UI_CURSOR_POINTER);
    return hover && nk_input_is_mouse_click_in_rect(&context->input,
        NK_BUTTON_LEFT, bounds);
}

bool bongo_cat_pref_toggle_float_config(struct nk_context *context, const char *id,
    const char *title, const char *unit, bool *enabled, float minimum,
    float *value, float maximum, float step, float default_value,
    const char *detail, bool *config_clicked) {
    int lines = bongo_cat_pref_detail_lines(context, detail);
    FormStyle saved;
    if (!bongo_cat_pref_form_begin(context, id, lines, &saved)) return false;
    if (config_clicked) *config_clicked = false;
    float available = nk_window_get_content_region(context).w;
    bool configured = *enabled;
    int columns = *enabled ? 4 : 2;
    float input_width = *enabled ? 122.0f : 0.0f;
    float config_width = configured ? 36.0f : 0.0f;
    float spacing = 8.0f * (columns - 1);
    float left = NK_MAX(220.0f,
        available - config_width - input_width - 80.0f - spacing);
    nk_layout_row_begin(context, NK_STATIC, 36, columns);
    nk_layout_row_push(context, left);
    char label[256];
    if (unit && unit[0]) {
        snprintf(label, sizeof(label), "%s (%s)", title, unit);
        title = label;
    }
    bongo_cat_pref_form_label(context, title);
    bool changed = false;
    if (configured) {
        nk_layout_row_push(context, config_width);
        if (bongo_cat_pref_config_button(context) && config_clicked)
            *config_clicked = true;
    }
    if (*enabled) {
        nk_layout_row_push(context, input_width);
        changed = bongo_cat_pref_control_float(context, id,
            minimum, value, maximum, step, default_value);
    }
    nk_layout_row_push(context, NK_MAX(80.0f,
        available - left - config_width - input_width - spacing));
    changed = bongo_cat_pref_control_toggle(context, id, enabled) || changed;
    nk_layout_row_end(context);
    bongo_cat_pref_description(context, detail, lines);
    bongo_cat_pref_form_end(context, &saved);
    return changed;
}