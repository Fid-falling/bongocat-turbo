/*
 * 侧边栏导航标签页。
 *
 * 高度是"反推"出来的: 用窗口可用高度先减掉品牌页头, 剩下的平均分给 count 个标签,
 * 再把单格夹在 [40, 68] 之间; 多出来的空隙 gap 平均分配在每格上下 —— 这就是
 * top_padding + i * gap 那一串偏移的来历。这样窗口拉高时标签会一起变高 (而不是留
 * 一大片空白), 但不会高到失去比例。count 必须 > 0, 否则 slot_height 除零。
 *
 * 两个必须成对的不变量:
 *  - hit 是"画出来的 tile 向外扩 10/1 像素"的判定框, 必须和绘制用同一个矩形;
 *  - 悬停时 tile 上移 hover_weight 像素, 命中框 (hit) 不跟着动 —— 否则鼠标停在
 *    边缘会出现"一动就脱离悬停"的抖动。
 *
 * 动画 id 用 snprintf 拼出 "sidebar-active-N"/"sidebar-hover-N": 状态表按字符串
 * 查键, 所以 id 必须在两帧之间完全一致 (见 ui_animation.c 的说明)。
 */
#include "ui_catime.h"
#include "ui_animation.h"
#include "ui_backend.h"
#include "ui_paint.h"

#include <stdio.h>

static void centered(struct nk_command_buffer *canvas,
    struct nk_rect bounds, const char *text, int length,
    const struct nk_user_font *font, struct nk_color color) {
    float width = font->width(font->userdata, font->height, text, length);
    float target_width = NK_MIN(bounds.w, width + 1.0f);
    struct nk_rect target = nk_rect(bounds.x + (bounds.w - target_width) * .5f,
        bounds.y + (bounds.h - font->height) * .5f,
        target_width, font->height);
    nk_draw_text(canvas, target, text, length, font,
        nk_rgba(0, 0, 0, 0), color);
}

/* 标签放不下时按空格折成两行, 挑"两行里较长那行最短"的切点 —— 比按字符数硬切好看,
   也比缩小字号稳 (字号是全局的)。找不到空格 (纯中文标签) 就退回单行居中, 宁可
   左右各切一点, 也不要中间突然换行。 */
static void nav_label(struct nk_command_buffer *canvas, struct nk_rect bounds,
    const char *text, const struct nk_user_font *font, struct nk_color color) {
    int length = nk_strlen(text);
    if (font->width(font->userdata, font->height, text, length) <= bounds.w) {
        centered(canvas, bounds, text, length, font, color);
        return;
    }
    int split = -1;
    float best = 1.0e30f;
    for (int i = 1; i + 1 < length; ++i) {
        if (text[i] != ' ') continue;
        float left = font->width(font->userdata, font->height, text, i);
        float right = font->width(font->userdata, font->height,
            text + i + 1, length - i - 1);
        float widest = NK_MAX(left, right);
        if (widest < best) { best = widest; split = i; }
    }
    if (split < 0) {
        centered(canvas, bounds, text, length, font, color);
        return;
    }
    struct nk_rect line = nk_rect(bounds.x, bounds.y,
        bounds.w, font->height);
    centered(canvas, line, text, split, font, color);
    line.y += font->height;
    centered(canvas, line, text + split + 1,
        length - split - 1, font, color);
}

static void tab_content(struct nk_command_buffer *canvas,
    struct nk_context *context, struct nk_rect tile, const char *label,
    int icon, struct nk_color color, BongoCatUIIconDraw draw_icon,
    void *icon_userdata) {
    const struct nk_user_font *font = bongo_cat_ui_caption_font(context);
    bool compact = tile.h < 64.0f;
    float icon_size = compact ? 20.0f : 24.0f;
    float icon_y = compact ? tile.y + 4.0f : tile.y + 5.0f;
    if (tile.w < 80.0f)
        icon_y = tile.y + (tile.h - icon_size) * .5f;
    struct nk_rect icon_bounds = nk_rect(
        tile.x + (tile.w - icon_size) * .5f, icon_y, icon_size, icon_size);
    if (draw_icon) draw_icon(icon_userdata, canvas, icon, icon_bounds, color);
    else bongo_cat_ui_fallback_icon(canvas, icon, tile, color);
    if (tile.w >= 80.0f) {
        struct nk_rect label_bounds = compact ? nk_rect(tile.x,
            tile.y + tile.h - font->height - 3.0f, tile.w, font->height) :
            nk_rect(tile.x, tile.y + 31.0f, tile.w, 36.0f);
        nav_label(canvas, label_bounds, label, font, color);
    } else if (nk_input_is_mouse_hovering_rect(&context->input, tile))
        nk_tooltip(context, label);
}

void bongo_cat_ui_tabs(struct nk_context *context, const char *const *labels,
    const int *icons, int count, int *active, bool interactive, bool dark,
    float available_height, bool about_badge,
    BongoCatUIIconDraw draw_icon, void *icon_userdata) {
    BongoCatUIPalette p = bongo_cat_ui_palette(dark);
    struct nk_command_buffer *canvas = nk_window_get_canvas(context);
    float header_height = nk_window_get_content_region(context).w < 100.0f ?
        118.0f : 148.0f;
    float room = NK_MAX(1.0f, available_height - header_height);
    float slot_height = room / count;
    float row_height = NK_CLAMP(40.0f, slot_height - 8.0f, 68.0f);
    float gap = NK_MAX(0.0f, slot_height - row_height);
    float top_padding = gap * .5f;
    for (int i = 0; i < count; ++i) {
        struct nk_rect bounds;
        nk_layout_row_dynamic(context, row_height, 1);
        if (nk_widget(&bounds, context) == NK_WIDGET_INVALID) continue;
        struct nk_rect hit = nk_rect(bounds.x + 10, bounds.y + 1,
            bounds.w - 20, bounds.h - 1);
        hit.y += top_padding + i * gap;
        bool hover = interactive &&
            nk_input_is_mouse_hovering_rect(&context->input, hit);
        bool selected = *active == i;
        char selection_id[32], hover_id[32];
        snprintf(selection_id, sizeof(selection_id), "sidebar-active-%d", i);
        snprintf(hover_id, sizeof(hover_id), "sidebar-hover-%d", i);
        float weight = bongo_cat_ui_animate_eased(context, selection_id,
            selected ? 1.0f : 0.0f, 200.0f, BONGO_CAT_UI_EASE_SWIFT);
        float hover_weight = bongo_cat_ui_animate_eased(context, hover_id,
            hover ? 1.0f : 0.0f, 200.0f, BONGO_CAT_UI_EASE_SWIFT);
        struct nk_rect tile = hit;
        tile.y -= hover_weight;
        if (hover) nk_fill_rect(canvas, tile, 8, p.hover);
        if (selected && p.effects) bongo_cat_ui_paint_shadow(context,
            tile, 8, 0, 5, 14, 0,
            nk_rgba(p.pink.r, p.pink.g, p.pink.b, 89));
        if (weight > 0.0f) nk_fill_rect(canvas, tile, 8,
            nk_rgba(p.pink.r, p.pink.g, p.pink.b, (nk_byte)(255 * weight)));
        struct nk_color color = selected ? nk_rgb(255, 255, 255) :
            (hover ? p.accent : p.muted);
        tab_content(canvas, context, tile, labels[i], icons ? icons[i] : i, color,
            draw_icon, icon_userdata);
        if (about_badge && i == count - 1) {
            nk_fill_circle(canvas, nk_rect(tile.x + tile.w - 11.0f,
                tile.y + 7.0f, 7.0f, 7.0f), p.danger);
        }
        if (hover) bongo_cat_ui_cursor_hover_rect(context, hit,
            BONGO_CAT_UI_CURSOR_POINTER);
        if (hover && nk_input_is_mouse_click_in_rect(&context->input,
            NK_BUTTON_LEFT, hit)) *active = i;
    }
}
