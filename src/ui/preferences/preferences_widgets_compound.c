/*
 * 复合控件: "开关 + 可选数值输入"排在一行 (例如"随机表情" + 间隔秒数)。
 * 关键点是列数随开关状态变: 关掉时是 2 列 (标题 + 开关), 打开时是 3 列
 * (标题 + 数值 + 开关) —— 数值框不是"禁用"而是整个不占位, 这样关掉时开关会靠到
 * 右边, 视觉上不会有空洞。
 * 两处 changed 用 || 合并: 用户可能在同一帧里既拖了数值又点了开关, 漏一个就会丢改动。
 */
#include "preferences_widgets.h"
#include "preferences_widgets_internal.h"
#include "preferences_controls.h"

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
