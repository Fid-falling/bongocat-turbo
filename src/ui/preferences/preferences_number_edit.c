/*
 * 步进器中间那个可以直接输入的数字框。
 *
 * 它把 Nuklear 的 nk_edit_string 套进一块**已经分配好的**矩形里: 用
 * nk_layout_space_begin + 保存/还原 panel 的办法, 让编辑框覆盖在既有控件之上而不额外
 * 占一行 (见中间那段 Overlay 注释)。这是本文件最绕的地方 —— 改动布局相关代码时,
 * saved_panel 的保存/还原必须保留, 否则整页排版会错位。
 *
 * 校验策略是"宽松输入 + 严格提交": 输入过程中只过滤字符 (number_filter), 允许用户
 * 打出 "-" 或 "." 这种中间态; 每次变化后再整体校验一次 (valid_text), 不合法就回滚到
 * 上一次的文本 —— 必须这样, 因为剪贴板粘贴进来的内容不经过 number_filter。
 * 全局只有一份编辑状态 (number_edit), 即同一时间只能有一个数字框在编辑。
 */
#include "preferences_number_edit.h"
#include "ui_backend.h"
#include "ui_catime.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * frame 记录上一次绘制的帧号: 隔了一帧还没再画到它, 说明这一行被滚出视野或被折叠,
 * 必须结束编辑, 否则会留下一个看不见却仍在吃键盘的输入焦点。
 */
static struct {
    struct nk_context *context;
    char id[64];
    char text[64];
    int length;
    unsigned int frame;
    bool integer;
    bool negative;
} number_edit;

void bongo_cat_pref_number_edit_reset(struct nk_context *context) {
    if (number_edit.context == context)
        memset(&number_edit, 0, sizeof(number_edit));
}

/* 输入过滤只挡"绝对不该出现的字符"; 负号与小数点是否允许由当前字段的 range/类型决定 */
static nk_bool number_filter(const struct nk_text_edit *edit, nk_rune rune) {
    (void)edit;
    return (rune >= '0' && rune <= '9') ||
        (rune == '-' && number_edit.negative) ||
        (rune == '.' && !number_edit.integer);
}

/*
 * 整体校验: 可选负号 + 数字, 浮点还允许最多一个小数点 (多一个就算非法)。
 * "1." / "-" 这类中间态是合法的, 所以用户输到一半不会被回滚。
 */
static bool valid_text(const char *text, bool integer, bool negative) {
    if (*text == '-' && negative) ++text;
    bool dot = false;
    for (; *text; ++text) {
        if (*text >= '0' && *text <= '9') continue;
        if (*text == '.' && !integer && !dot) { dot = true; continue; }
        return false;
    }
    return true;
}

bool bongo_cat_pref_number_edit(struct nk_context *context, const char *id,
    struct nk_rect bounds, const char *formatted, bool integer,
    double minimum, double maximum, double *value) {
    bool hover = nk_input_is_mouse_hovering_rect(&context->input, bounds);
    bool pressed = nk_input_is_mouse_pressed(&context->input, NK_BUTTON_LEFT);
    bool active = number_edit.context == context &&
        strcmp(number_edit.id, id) == 0;
    if (active && ((pressed && !hover) ||
        (unsigned int)(context->seq - number_edit.frame) > 1)) {
        bongo_cat_pref_number_edit_reset(context);
        nk_edit_unfocus(context);
        active = false;
    }
    bool begin = !active && pressed && hover;
    if (begin) {
        number_edit.context = context;
        snprintf(number_edit.id, sizeof(number_edit.id), "%s", id);
        snprintf(number_edit.text, sizeof(number_edit.text), "%s", formatted);
        number_edit.length = (int)strlen(number_edit.text);
        number_edit.integer = integer;
        number_edit.negative = minimum < 0;
        active = true;
        BongoCatUIBackend *backend = bongo_cat_ui_backend_for_context(context);
        if (backend) SDL_StartTextInput(backend->window);
    }
    if (hover) bongo_cat_ui_cursor_hover_rect(context, bounds,
        BONGO_CAT_UI_CURSOR_TEXT);
    if (!active) return false;
    number_edit.frame = context->seq;

    char previous[sizeof(number_edit.text)];
    memcpy(previous, number_edit.text, sizeof(previous));
    int previous_length = number_edit.length;
    BongoCatUIPalette p = bongo_cat_ui_palette(bongo_cat_ui_dark(context));
    struct nk_style_edit saved_style = context->style.edit;
    const struct nk_user_font *saved_font = context->style.font;
    context->style.font = bongo_cat_ui_body_font(context);
    context->style.edit.normal = nk_style_item_color(p.field);
    context->style.edit.hover = nk_style_item_color(p.field);
    context->style.edit.active = nk_style_item_color(p.field);
    context->style.edit.text_normal = p.text;
    context->style.edit.text_hover = p.text;
    context->style.edit.text_active = p.text;
    context->style.edit.selected_normal = nk_rgb(83, 172, 252);
    context->style.edit.selected_hover = nk_rgb(83, 172, 252);
    context->style.edit.selected_text_normal = nk_rgb(255, 255, 255);
    context->style.edit.selected_text_hover = nk_rgb(255, 255, 255);
    context->style.edit.cursor_normal = p.text;
    context->style.edit.cursor_hover = p.text;
    context->style.edit.border = 0;
    context->style.edit.rounding = 0;
    context->style.edit.padding = nk_vec2(2, 0);

    /* Overlay the editor in an already allocated widget without advancing its row. */
    struct nk_panel saved_panel = *context->current->layout;
    nk_layout_space_begin(context, NK_STATIC, bounds.h, 1);
    struct nk_vec2 local = nk_layout_space_to_local(context,
        nk_vec2(bounds.x, bounds.y));
    nk_layout_space_push(context, nk_rect(local.x, local.y, bounds.w, bounds.h));
    if (begin) nk_edit_unfocus(context);
    nk_flags flags = nk_edit_string(context,
        (nk_flags)NK_EDIT_FIELD | (nk_flags)NK_EDIT_AUTO_SELECT |
            (nk_flags)NK_EDIT_SIG_ENTER,
        number_edit.text, &number_edit.length, sizeof(number_edit.text) - 1,
        number_filter);
    nk_layout_space_end(context);
    *context->current->layout = saved_panel;
    context->style.edit = saved_style;
    context->style.font = saved_font;

    number_edit.text[number_edit.length] = '\0';
    /* Clipboard insertion also needs whole-value validation. */
    if (!valid_text(number_edit.text, integer, minimum < 0)) {
        memcpy(number_edit.text, previous, sizeof(previous));
        number_edit.length = previous_length;
    }
    char *end;
    /* 只有"文本真的变了、能完整解析成数字、且不是 NaN/Inf"才写回值: 输入到一半
     * ("-", "1.") 时 strtod 也能解析, 但那是中间态, 所以用文本变化做闸门 */
    double parsed = strtod(number_edit.text, &end);
    if (strcmp(previous, number_edit.text) != 0 &&
        end != number_edit.text && !*end && isfinite(parsed))
        *value = NK_CLAMP(minimum, parsed, maximum);
    if (flags & (NK_EDIT_COMMITED | NK_EDIT_DEACTIVATED)) {
        nk_edit_unfocus(context);
        bongo_cat_pref_number_edit_reset(context);
    }
    return true;
}
