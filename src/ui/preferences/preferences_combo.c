/*
 * 下拉选择器 (combo)。
 *
 * 这是在 Nuklear 自带 combo 之上"重画一遍外观"的实现: 触发框、弹出菜单、每一项都
 * 由这里手绘, 借用的只是 nk_combo_begin_label/nk_combo_end 提供的弹层与滚动机制。
 * 所以整段被两层样式保存-修改-还原包住 (combo / scrollv / window), 目的是先把
 * Nuklear 默认外观全部抹成透明, 再画自己的。任何一处漏还原, 这一帧之后所有控件
 * 都会变成"透明"的。
 *
 * 两个必须知道的细节:
 * - was_open 通过 popup.combo_count 与 popup.name 判断"这个下拉现在是开着的",
 * 这是 Nuklear 暴露"当前弹层属于谁"的唯一途径;
 * - 菜单的展开动画是靠 transform_item 把每一项的矩形按 scale 从弹层原点缩放出来
 * 的, 不是靠容器动画 —— 所以每一项都要各自变换一次。
 */
#include "preferences_controls.h"
#include "ui_animation.h"
#include "ui_backend.h"
#include "ui_catime.h"
#include "ui_paint.h"

#include <math.h>
#include <stdio.h>

static void text(struct nk_command_buffer *canvas, struct nk_rect bounds,
    const char *value, const struct nk_user_font *font, struct nk_color color) {
    nk_draw_text(canvas, bounds, value, nk_strlen(value), font,
        nk_rgba(0, 0, 0, 0), color);
}

/*
 * 箭头角度 = 180 度 x open: open 是 0..1 的补间值, 所以箭头会随菜单一起平滑翻转,
 * 而不是瞬间朝上/朝下。
 */
static void chevron(struct nk_command_buffer *canvas, struct nk_rect bounds,
    struct nk_color color, float open) {
    float x = bounds.x + bounds.w - 17, y = bounds.y + bounds.h * .5f;
    float angle = 3.14159265f * open, cosine = cosf(angle), sine = sinf(angle);
    float ax = -4 * cosine + 3 * sine, ay = -4 * sine - 3 * cosine;
    float bx = -2 * sine, by = 2 * cosine;
    float cx = 4 * cosine + 3 * sine, cy = 4 * sine - 3 * cosine;
    nk_stroke_line(canvas, x + ax, y + ay, x + bx, y + by, 2, color);
    nk_stroke_line(canvas, x + bx, y + by, x + cx, y + cy, 2, color);
}

/* 触发框: 外扩 3px 的一圈悬停底色 + 本体 + 描边 + 当前选项 + 箭头 */
static void trigger(struct nk_context *context, struct nk_command_buffer *canvas,
    struct nk_rect bounds, const char *label, BongoCatUIPalette p,
    float hover, float open) {
    if (hover > .001f) nk_fill_rect(canvas,
        nk_rect(bounds.x - 3, bounds.y - 3, bounds.w + 6, bounds.h + 6), 13,
        bongo_cat_ui_color_mix(p.surface, p.hover, hover));
    nk_fill_rect(canvas, bounds, 10,
        bongo_cat_ui_color_mix(p.field, p.surface, hover));
    nk_stroke_rect(canvas, bounds, 10, 1,
        bongo_cat_ui_color_mix(p.border_subtle, p.accent, hover));
    const struct nk_user_font *font = bongo_cat_ui_body_font(context);
    text(canvas, nk_rect(bounds.x + 14,
        bounds.y + (bounds.h - font->height) * .5f, bounds.w - 42,
        font->height), label, font, p.text);
    chevron(canvas, bounds, bongo_cat_ui_color_mix(p.muted, p.accent,
        hover), open);
}

/*
 * 把 Nuklear 排出来的项矩形映射到"缩放后的菜单"坐标系: 展开动画期间菜单比最终
 * 尺寸小, 每一项都要跟着缩, 否则会看到文字先大后小地跳一下。
 */
static struct nk_rect transform_item(struct nk_rect item,
    struct nk_rect popup, struct nk_rect menu, float scale) {
    item.x = menu.x + (item.x - popup.x) * scale;
    item.y = menu.y + (item.y - popup.y) * scale;
    item.w *= scale; item.h *= scale;
    return item;
}

static bool draw_item(struct nk_context *context, struct nk_rect bounds,
    const char *label, bool selected, BongoCatUIPalette p, float opacity) {
    bool hover = nk_input_is_mouse_hovering_rect(&context->input, bounds);
    struct nk_command_buffer *canvas = nk_window_get_canvas(context);
    if (selected || hover) nk_fill_rect(canvas, bounds, 8,
        bongo_cat_ui_color_alpha(p.selection, opacity));
    const struct nk_user_font *font = bongo_cat_ui_body_font(context);
    text(canvas, nk_rect(bounds.x + 12,
        bounds.y + (bounds.h - font->height) * .5f, bounds.w - 22,
        font->height), label, font, bongo_cat_ui_color_alpha(
        selected || hover ? p.accent : p.text, opacity));
    if (hover) bongo_cat_ui_cursor_hover_rect(context, bounds,
        BONGO_CAT_UI_CURSOR_POINTER);
    return hover && nk_input_is_mouse_click_in_rect(&context->input,
        NK_BUTTON_LEFT, bounds);
}

/*
 * 菜单背景。scissor 外扩是为了让阴影画得出来, 画完必须还原成内容区 —— 不还原的话
 * 后面所有绘制都会被裁在这块矩形里。
 */
static void menu_background(struct nk_context *context, struct nk_rect menu,
    struct nk_rect shadow, BongoCatUIPalette p, float opacity) {
    struct nk_command_buffer *canvas = nk_window_get_canvas(context);
    nk_push_scissor(canvas, nk_rect(menu.x - 64, menu.y - 48,
        menu.w + 128, menu.h + 112));
    if (p.effects) {
        struct nk_color dark = bongo_cat_ui_dark(context) ?
            nk_rgba(0, 0, 0, 92) : nk_rgba(24, 34, 48, 46);
        bongo_cat_ui_paint_shadow(context, shadow, 12, 0, 14, 32, 0,
            bongo_cat_ui_color_alpha(dark, opacity));
        bongo_cat_ui_paint_shadow(context, shadow, 12, 0, 4, 12, 0,
            nk_rgba(p.accent.r, p.accent.g, p.accent.b,
            (nk_byte)(89.0f * opacity)));
    }
    nk_fill_rect(canvas, menu, 12,
        bongo_cat_ui_color_alpha(p.surface, opacity));
    nk_stroke_rect(canvas, menu, 12, 1,
        bongo_cat_ui_color_alpha(p.border, opacity));
    nk_push_scissor(canvas, nk_window_get_content_region(context));
}

/*
 * 下拉的宽度固定 156px (但不会超出可用宽度), 右对齐到表单行右侧 —— 和滑杆、步进器
 * 保持同一条右边界。
 * selected 越界一律夹回合法范围: 配置里可能存着一个已经删掉的选项下标。
 */
int bongo_cat_pref_control_combo(struct nk_context *context, const char *id,
    const char *const *items, int count, int selected) {
    if (count <= 0) return selected;
    selected = NK_CLAMP(0, selected, count - 1);
    BongoCatUIPalette p = bongo_cat_ui_palette(bongo_cat_ui_dark(context));
    struct nk_rect widget = nk_widget_bounds(context);
    struct nk_rect bounds = nk_rect(widget.x + NK_MAX(0.0f,
        widget.w - 156.0f - 6.0f),
        widget.y - 1, NK_MIN(156.0f, widget.w), 38);
    struct nk_command_buffer *parent = nk_window_get_canvas(context);
    bool hover = nk_input_is_mouse_hovering_rect(&context->input, bounds);
    nk_hash combo_index = context->current->popup.combo_count;
    bool was_open = context->current->popup.win &&
        context->current->popup.type == NK_PANEL_COMBO &&
        context->current->popup.name == combo_index;
    char hover_id[80], open_id[80], scroll_id[80];
    snprintf(hover_id, sizeof(hover_id), "combo-hover-%s", id);
    snprintf(open_id, sizeof(open_id), "combo-open-%s", id);
    snprintf(scroll_id, sizeof(scroll_id), "combo-scroll-%s", id);
    float hover_amount = bongo_cat_ui_animate_eased(context, hover_id,
        was_open || hover ? 1.0f : 0.0f, 200, BONGO_CAT_UI_EASE_STANDARD);
    float open_amount = bongo_cat_ui_animate_eased(context, open_id,
        was_open ? 1.0f : 0.0f, 160, BONGO_CAT_UI_EASE_STANDARD);
    trigger(context, parent, bounds, items[selected], p, hover_amount, open_amount);
    if (hover) bongo_cat_ui_cursor_hover_rect(context, bounds,
        BONGO_CAT_UI_CURSOR_POINTER);
    struct nk_style_combo saved_combo = context->style.combo;
    struct nk_style_scrollbar saved_scrollv = context->style.scrollv;
    struct nk_style_window saved_window = context->style.window;
    struct nk_color clear = nk_rgba(0, 0, 0, 0);
    context->style.combo.normal = context->style.combo.hover =
        context->style.combo.active = nk_style_item_color(clear);
    context->style.combo.border = 0;
    context->style.combo.label_normal = context->style.combo.label_hover =
        context->style.combo.label_active = clear;
    context->style.combo.sym_normal = context->style.combo.sym_hover =
        context->style.combo.sym_active = NK_SYMBOL_NONE;
    context->style.window.fixed_background = nk_style_item_color(clear);
    context->style.window.background = context->style.window.border_color = clear;
    context->style.window.combo_border_color = clear;
    context->style.window.combo_border = 0;
    context->style.window.combo_padding = nk_vec2(6, 6);
    context->style.window.spacing = nk_vec2(0, 0);
    float menu_height = NK_MIN(236.0f, 12.0f + count * 36.0f);
    struct nk_panel *root = context->current->layout;
    while (root->parent) root = root->parent;
    /*
     * 弹层高度要按"触发框下面还剩多少可见空间"夹一次: Nuklear 的弹层不会被窗口
     * 自动裁剪到滚动视口里, 不夹的话在页面底部打开的菜单会有一半跑到窗口外点不到。
     * clip 取的是最外层布局的裁剪区 (一路 parent 上去), 不是当前组的。
     */
    struct nk_rect clip = root->clip;
    float visible_below = clip.y + clip.h - (bounds.y + bounds.h) - 6.0f;
    float popup_height = NK_MIN(menu_height + 8.0f,
        NK_MAX(72.0f, visible_below));
    bool open = nk_combo_begin_label(context, items[selected],
        nk_vec2(widget.w, popup_height));
    if (open) {
        struct nk_rect popup = nk_window_get_bounds(context);
        bool wheel_scrolling = context->input.mouse.scroll_delta.y != 0.0f;
        float scroll_amount = bongo_cat_ui_animate_pulse(context, scroll_id,
            wheel_scrolling, 180, 160);
        struct nk_rect scrollbar_hit = nk_rect(popup.x + popup.w - 14,
            popup.y, 14, popup.h);
        bool scrollbar_hover = nk_input_is_mouse_hovering_rect(
            &context->input, scrollbar_hit);
        context->style.scrollv.padding.x = scrollbar_hover ? 0.0f : 2.0f;
        context->style.scrollv.cursor_hover = nk_style_item_color(p.accent);
        context->style.scrollv.cursor_active = nk_style_item_color(p.accent);
        if (scroll_amount > .001f)
            context->style.scrollv.cursor_normal = nk_style_item_color(p.accent);
        float scale = .98f + .02f * open_amount;
        struct nk_rect menu = nk_rect(popup.x + popup.w * (1.0f - scale),
            popup.y + 8.0f - 6.0f * (1.0f - open_amount), popup.w * scale,
            (popup.h - 8.0f) * scale);
        struct nk_rect shadow = nk_rect(popup.x, menu.y, popup.w, popup.h - 8.0f);
        menu_background(context, menu, shadow, p, open_amount);
        nk_layout_row_dynamic(context, 36, 1);
        for (int i = 0; i < count; ++i) {
            struct nk_rect item;
            if (nk_widget(&item, context) == NK_WIDGET_INVALID) continue;
            item = transform_item(item, popup, menu, scale);
            /*
             * 选中一项后立刻组合关闭: 不等 Nuklear 的"点击别处自动关", 那样菜单会
             * 多停留一帧, 看起来像没点上。
             */
            if (draw_item(context, item, items[i], i == selected, p,
                open_amount)) { selected = i; nk_combo_close(context); }
        }
        nk_combo_end(context);
    }
    context->style.combo = saved_combo;
    context->style.scrollv = saved_scrollv;
    context->style.window = saved_window;
    return selected;
}
