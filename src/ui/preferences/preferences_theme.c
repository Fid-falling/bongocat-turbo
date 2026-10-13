/*
 * 主题/渲染器/帧率这一组"分段选择器" (capsule) 与它外面的表单壳。
 *
 * form_begin / form_end 是成对的作用域: 把全局的 Nuklear 窗口样式临时改成"无底色、
 * 无边框、13/11 内边距"来画一行表单, 画完还原。之所以要清掉底色, 是因为行本身画在
 * 页面背景上, 再叠一层窗口底色会脏。
 * 三个特效区都是手绘的 (电脑/太阳/月亮), 不走图标图集: 它们要跟随选中动画改颜色,
 * 而图集是按固定贴图着色画的, 做不到"图标底色随滑块一起渐变"。手绘与"这段被点了吗"
 * 的判定都在 preferences_segment.c (本文件贴着 500 行上限, 见 cmake/CheckLines.cmake)。
 * 动画 id 全都带 id 参数前缀 —— 同一个窗口里有多个分段控件, 不区分会互相带动画。
 */
#include "preferences_theme.h"
#include "preferences_widgets_internal.h"
#include "ui_animation.h"
#include "ui_backend.h"
#include "ui_catime.h"
#include "ui_paint.h"
#include "bongo_cat/config.h"

#include <math.h>
#include <stdio.h>

typedef struct ThemeFormStyle {
    struct nk_style_item background;
    struct nk_color window_color;
    struct nk_color border_color;
    struct nk_vec2 padding;
    struct nk_vec2 spacing;
    float border;
} ThemeFormStyle;

/*
 * 进入一行表单。返回 true 表示组已开好, 调用方结束时必须配 form_end;
 * 返回 false 表示这一行被滚动裁剪掉了 —— 此时组没开成, 但样式已经复原 (见中间那段),
 * 所以调用方直接返回即可, 不要再调 form_end。
 */
static bool form_begin(struct nk_context *context, const char *id,
    ThemeFormStyle *saved) {
    saved->background = context->style.window.fixed_background;
    saved->window_color = context->style.window.background;
    saved->border_color = context->style.window.group_border_color;
    saved->padding = context->style.window.group_padding;
    saved->spacing = context->style.window.spacing;
    saved->border = context->style.window.group_border;
    struct nk_color clear = nk_rgba(0, 0, 0, 0);
    context->style.window.fixed_background = nk_style_item_color(clear);
    context->style.window.background = clear;
    context->style.window.group_border_color = clear;
    context->style.window.group_padding = nk_vec2(13, 11);
    context->style.window.spacing = nk_vec2(8, 3);
    context->style.window.group_border = 0;
    nk_layout_row_dynamic(context, 76, 1);
    if (nk_group_begin(context, id, NK_WINDOW_NO_SCROLLBAR)) return true;
    context->style.window.fixed_background = saved->background;
    context->style.window.background = saved->window_color;
    context->style.window.group_border_color = saved->border_color;
    context->style.window.group_padding = saved->padding;
    context->style.window.spacing = saved->spacing;
    context->style.window.group_border = saved->border;
    bongo_cat_pref_row_icon_clear(context);
    return false;
}

/* 收尾: 先让行图标状态复位再关组, 顺序反了会让图标残留到下一行 */
static void form_end(struct nk_context *context, const ThemeFormStyle *saved) {
    bongo_cat_pref_row_icon_clear(context);
    nk_group_end(context);
    context->style.window.fixed_background = saved->background;
    context->style.window.background = saved->window_color;
    context->style.window.group_border_color = saved->border_color;
    context->style.window.group_padding = saved->padding;
    context->style.window.spacing = saved->spacing;
    context->style.window.group_border = saved->border;
}

/*
 * 分段选择器 (胶囊控件)。icons 为真时画图标, 否则画文字。
 * 三个动画层次的配合要看清:
 * position       选中段的浮点位置 (0..count-1), 用补间值而不是整数下标, 滑动条
 * 才能平滑移动;
 * outer_hover    整个控件的悬停描边;
 * theme-hover-id-i 每一段自己的悬停底色。
 * active_weight 是"离选中位置多远"的权重, 用它把图标/文字颜色推向白色并放大 1px,
 * 这样滑块移动过程中途经的段会有一次淡淡的提亮, 而不是硬切。
 * disabled 段 (比如不可用的 Vulkan) 连悬停高亮都不给 —— 只禁用点击但保留高亮,
 * 会让人以为点得动。
 *
 * clicked 是"用户实际点在哪一段上"的出口参数 (可以是 NULL), 它**包含禁用段**。
 * 为什么要单独上报: 禁用段不改选中值, 调用方只看返回值就永远不知道用户点过它, 于是
 * "点了没反应"的路径连一条日志、一句提示都留不下来 —— 设置页的渲染后端那一行必须能
 * 对不可用的后端说明原因 (见 preferences_renderer_row.c)。
 *
 * 图标与"这一段被点了吗"的判定都在 preferences_segment.c (本文件贴着 500 行上限)。
 * 点击判定必须按控件去重, 判据和踩过的坑见那里的注释 —— 共用一份状态会让一帧里排在
 * 后面的分段控件永远收不到点击。
 */
static int capsule(struct nk_context *context, const char *id,
    const char *const *labels, int count, bool icons, int selected,
    float width, int disabled, int *clicked) {
    struct nk_rect widget;
    if (nk_widget(&widget, context) == NK_WIDGET_INVALID) return selected;
    selected = NK_CLAMP(0, selected, count - 1);
    struct nk_rect bounds = nk_rect(widget.x + NK_MAX(0.0f,
        widget.w - width - 6.0f), widget.y - 1, NK_MIN(width, widget.w), 38);
    float segment = bounds.w / count;
    bool hover = nk_input_is_mouse_hovering_rect(&context->input, bounds);
    int raw_hovered = hover ? NK_CLAMP(0, (int)((context->input.mouse.pos.x -
        bounds.x) / segment), count - 1) : -1;
    /* 禁用段既不亮也不可点, 连悬停高亮一起掐掉; 但点击要上报 (见函数头注释) */
    int hovered = raw_hovered == disabled ? -1 : raw_hovered;
    if (bongo_cat_pref_segment_click(context, id, hover) && raw_hovered >= 0) {
        if (clicked) *clicked = raw_hovered;
        if (raw_hovered != disabled) selected = raw_hovered;
    }

    char animation_id[96];
    snprintf(animation_id, sizeof(animation_id), "theme-selection-%s", id);
    float position = bongo_cat_ui_animate_eased(context, animation_id,
        (float)selected, 220, BONGO_CAT_UI_EASE_SWIFT);
    snprintf(animation_id, sizeof(animation_id), "theme-hover-%s", id);
    float outer_hover = bongo_cat_ui_animate_eased(context, animation_id,
        hover ? 1.0f : 0.0f, 180, BONGO_CAT_UI_EASE_STANDARD);
    BongoCatUIPalette p = bongo_cat_ui_palette(bongo_cat_ui_dark(context));
    struct nk_command_buffer *canvas = nk_window_get_canvas(context);
    nk_fill_rect(canvas, bounds, 14, p.field);
    for (int i = 0; i < count; ++i) {
        snprintf(animation_id, sizeof(animation_id), "theme-hover-%s-%d", id, i);
        float amount = bongo_cat_ui_animate_eased(context, animation_id,
            hovered == i ? 1.0f : 0.0f, 160, BONGO_CAT_UI_EASE_STANDARD);
        if (amount > .001f) nk_fill_rect(canvas,
            nk_rect(bounds.x + 3 + segment * i, bounds.y + 3,
                segment - 6, bounds.h - 6), 11,
            bongo_cat_ui_color_mix(p.field, p.selection, amount));
    }
    struct nk_rect active = nk_rect(bounds.x + 3 + segment * position,
        bounds.y + 3, segment - 6, bounds.h - 6);
    if (p.effects) bongo_cat_ui_paint_shadow(context, active, 11,
        0, 2, 7, 0, nk_rgba(p.accent.r, p.accent.g, p.accent.b, 76));
    nk_fill_rect(canvas, active, 11, p.accent);
    nk_stroke_rect(canvas, bounds, 14, 1,
        bongo_cat_ui_color_mix(p.border_subtle, p.accent, outer_hover));
    for (int i = 0; i < count; ++i) {
        snprintf(animation_id, sizeof(animation_id), "theme-hover-%s-%d", id, i);
        float amount = bongo_cat_ui_animate_eased(context, animation_id,
            hovered == i ? 1.0f : 0.0f, 160, BONGO_CAT_UI_EASE_STANDARD);
        float distance = fabsf(position - i);
        float active_weight = 1.0f - NK_CLAMP(0.0f,
            (distance - .28f) / .20f, 1.0f);
        struct nk_color base = bongo_cat_ui_color_mix(p.muted, p.accent, amount);
        struct nk_color color = bongo_cat_ui_color_mix(base,
            nk_rgb(255, 255, 255), active_weight);
        struct nk_color segment_background = bongo_cat_ui_color_mix(
            p.field, p.selection, amount);
        segment_background = bongo_cat_ui_color_mix(segment_background,
            p.accent, active_weight);
        float size = 20.0f + amount;
        struct nk_rect icon = nk_rect(bounds.x + segment * (i + .5f) - size * .5f,
            bounds.y + (bounds.h - size) * .5f, size, size);
        if (icons) bongo_cat_pref_segment_icon(canvas, i, icon, color,
            segment_background);
        else {
            const struct nk_user_font *font = context->style.font;
            int length = nk_strlen(labels[i]);
            float text_width = font->width(font->userdata, font->height,
                labels[i], length);
            nk_draw_text(canvas, nk_rect(bounds.x + segment * (i + .5f) -
                text_width * .5f, bounds.y + (bounds.h - font->height) * .5f,
                text_width, font->height), labels[i], length, font,
                nk_rgba(0, 0, 0, 0), color);
        }
    }
    if (hover && hovered >= 0) {
        bongo_cat_ui_cursor_hover_rect(context, bounds,
            BONGO_CAT_UI_CURSOR_POINTER);
    }
    return selected;
}

/*
 * 一行"左标签 + 右分段控件"。左右宽度是算出来的 (右列至少 control_width + 20),
 * 左列有 220px 下限: 窗口窄到一定程度时宁可让标签占满左边, 也不要把分段控件压扁
 * 到点不准。
 */
static int choice_row(struct nk_context *context, const char *id,
    const char *title, const char *const *labels, int count, bool icons,
    int selected, float control_width, int disabled, int *clicked) {
    ThemeFormStyle saved;
    if (!form_begin(context, id, &saved)) return selected;
    float available = nk_window_get_content_region(context).w;
    float left = NK_MAX(220.0f, available - control_width - 28.0f);
    nk_layout_row_begin(context, NK_STATIC, 36, 2);
    nk_layout_row_push(context, left);
    bongo_cat_pref_form_label(context, title);
    nk_layout_row_push(context, NK_MAX(control_width + 20.0f, available - left - 8.0f));
    selected = capsule(context, id, labels, count, icons, selected,
        control_width, disabled, clicked);
    nk_layout_row_end(context);
    form_end(context, &saved);
    return selected;
}

/* disabled 传 -1 表示没有禁用段 */
int bongo_cat_pref_theme(struct nk_context *context, const char *id,
    const char *title, const char *const *labels, int selected) {
    return choice_row(context, id, title, labels, 3, true, selected, 156.0f, -1,
        NULL);
}

/*
 * Vulkan 不可用的机器上把 Vulkan 段画成禁用; 越界的 renderer 值一律退回 Auto,
 * 免得旧配置里存了一个现在已经不存在的序号导致三个段全都不选中。
 *
 * clicked (可以是 NULL) 回报"用户点在哪一段上", 含被禁用的 Vulkan 段 —— 调用方要靠
 * 它给"点了没反应"留一条日志和一句用户可见的提示。
 */
int bongo_cat_pref_renderer(struct nk_context *context, const char *id,
    const char *title, int renderer, bool vulkan_selectable, int *clicked) {
    static const char *labels[] = {"Auto", "OpenGL", "Vulkan"};
    int selected = renderer >= 0 && renderer < BONGO_CAT_RENDERER_COUNT ?
        renderer : (int)BONGO_CAT_RENDERER_AUTO;
    return choice_row(context, id, title, labels, 3, false, selected, 192.0f,
        vulkan_selectable ? -1 : (int)BONGO_CAT_RENDERER_VULKAN, clicked);
}

/*
 * 界面上的三段是 30 / 60(默认) / 无限, 而存储用的是"实际帧率上限"数值, 所以
 * 进出都要做一次映射; fps 既不是 30 也不是默认值时就当成"无限"。
 */
int bongo_cat_pref_fps(struct nk_context *context, const char *id,
    const char *title, int fps, const char *unlimited_label) {
    /* 比主题行宽: "Unlimited" 比图标占地方, 沿用 156px 三段会挤到一起 */
    const char *labels[] = {"30 FPS", "60 FPS",
        unlimited_label && unlimited_label[0] ? unlimited_label : "Unlimited"};
    int selected = fps == 30 ? 0 : (fps == BONGO_CAT_DEFAULT_MAX_FPS ? 1 : 2);
    int picked = choice_row(context, id, title, labels, 3, false, selected, 192.0f,
        -1, NULL);
    return picked == 0 ? 30 : (picked == 1 ? BONGO_CAT_DEFAULT_MAX_FPS
        : BONGO_CAT_MAX_FPS_UNLIMITED);
}
