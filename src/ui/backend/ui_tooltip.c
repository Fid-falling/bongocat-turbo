/*
 * 带换行的提示气泡。Nuklear 自带的 nk_tooltip 不会折行, 长文案会顶出屏幕, 所以
 * 这里自己算宽度、自己按词断行 (中文没有空格, 退回"按字符断")。
 *
 * 两个不变量:
 *  - 先量自然宽度再决定左右哪边放得下; 两边都放不下时强制用整窗宽度并靠左 ——
 *    宁可窄一点, 也别把气泡推到窗口外面;
 *  - nk_tooltip_begin_offset 返回 false 表示这一帧没能开窗 (超出 Nuklear 的弹窗
 *    栈深度), 必须立刻 return, 不能继续调 nk_layout_*。
 */
#include "ui_tooltip.h"
#include "ui_backend.h"
#include "ui_catime.h"

#include <string.h>

static int tooltip_wrap_length(const struct nk_user_font *font,
    const char *text, int length, float width) {
    int consumed = 0;
    int last_space = 0;
    while (consumed < length) {
        nk_rune rune = 0;
        int glyph_length = nk_utf_decode(text + consumed, &rune,
            length - consumed);
        if (glyph_length <= 0) glyph_length = 1;
        int next = consumed + glyph_length;
        float measured = font->width(font->userdata, font->height,
            text, next);
        if (consumed > 0 && measured > width)
            return last_space > 0 ? last_space : consumed;
        consumed = next;
        if (rune == ' ') last_space = consumed;
    }
    return consumed > 0 ? consumed : length;
}

static void tooltip_multiline(struct nk_context *context, const char *text,
    struct nk_color color) {
    const struct nk_user_font *font = context->style.font;
    struct nk_vec2 window_size = nk_window_get_size(context);
    struct nk_vec2 mouse = context->input.mouse.pos;
    struct nk_vec2 offset = context->style.window.tooltip_offset;
    float margin = 8.0f;
    float outer_padding = context->style.window.padding.x * 4.0f;
    float left_space = mouse.x - offset.x - margin;
    float right_space = window_size.x - mouse.x - offset.x - margin;
    bool place_left = left_space > right_space;
    float available = place_left ? left_space : right_space;
    if (available <= outer_padding) {
        available = NK_MAX(1.0f, window_size.x - margin * 2.0f);
        place_left = false;
    }
    const char *segment = text;
    float natural_width = 0.0f;
    while (segment) {
        const char *end = strchr(segment, '\n');
        int length = end ? (int)(end - segment) : nk_strlen(segment);
        natural_width = NK_MAX(natural_width, font->width(font->userdata,
            font->height,
            segment, length));
        segment = end ? end + 1 : NULL;
    }
    float content_width = NK_MIN(natural_width,
        NK_MAX(1.0f, available - outer_padding));
    float width = content_width + outer_padding;
    enum nk_tooltip_pos position;
    bool place_above = mouse.y > window_size.y * .55f;
    if (place_above)
        position = place_left ? NK_BOTTOM_RIGHT : NK_BOTTOM_LEFT;
    else
        position = place_left ? NK_TOP_RIGHT : NK_TOP_LEFT;
    if (!nk_tooltip_begin_offset(context, width, position, offset)) return;
    float height = font->height + context->style.window.padding.y * 2.0f;
    segment = text;
    while (segment) {
        const char *end = strchr(segment, '\n');
        int length = end ? (int)(end - segment) : nk_strlen(segment);
        if (!length) {
            nk_layout_row_dynamic(context, height, 1);
            nk_text_colored(context, "", 0, NK_TEXT_LEFT, color);
        } else {
            while (length > 0) {
                int fitting = tooltip_wrap_length(font, segment, length,
                    content_width);
                nk_layout_row_dynamic(context, height, 1);
                nk_text_colored(context, segment, fitting, NK_TEXT_LEFT,
                    color);
                segment += fitting;
                length -= fitting;
            }
        }
        if (!end) break;
        segment = end ? end + 1 : NULL;
    }
    nk_tooltip_end(context);
}

void bongo_cat_ui_question_tooltip(struct nk_context *context,
    const char *question, const char *reply) {
    if (!context || !question || !question[0] || !reply || !reply[0]) return;
    nk_layout_row_dynamic(context, 19.0f, 1);
    struct nk_rect bounds;
    if (nk_widget(&bounds, context) == NK_WIDGET_INVALID) return;
    const struct nk_user_font *font = bongo_cat_ui_caption_font(context);
    BongoCatUIPalette palette = bongo_cat_ui_palette(bongo_cat_ui_dark(context));
    float question_width = font->width(font->userdata, font->height,
        question, nk_strlen(question));
    struct nk_rect text = nk_rect(bounds.x + 5.0f,
        bounds.y + (bounds.h - font->height) * .5f, question_width,
        font->height);
    bool hover = nk_input_is_mouse_hovering_rect(&context->input, text);
    if (hover) {
        bongo_cat_ui_cursor_hover_rect(context, text,
            BONGO_CAT_UI_CURSOR_POINTER);
    }
    nk_draw_text(nk_window_get_canvas(context), text, question,
        nk_strlen(question), font, nk_rgba(0, 0, 0, 0), palette.accent);
    if (hover) tooltip_multiline(context, reply, palette.pink);
}

void bongo_cat_ui_plain_tooltip(struct nk_context *context,
    const char *text, struct nk_color color) {
    if (!context || !text || !text[0]) return;
    /*
     * 设置窗口为了透明会把 window 背景设成全透明 (见 root_style_apply), 气泡复用同一份
     * 样式, 不临时补一块不透明底就会变成"字浮在封面/卡片上", 在有画面的地方读不清。
     * 只覆盖这一帧的气泡绘制, 画完立刻还原, 不影响其它窗口。
     */
    struct nk_style_item saved_fixed = context->style.window.fixed_background;
    struct nk_color saved_background = context->style.window.background;
    BongoCatUIPalette palette = bongo_cat_ui_palette(bongo_cat_ui_dark(context));
    context->style.window.fixed_background =
        nk_style_item_color(palette.surface);
    context->style.window.background = palette.surface;
    tooltip_multiline(context, text, color);
    context->style.window.fixed_background = saved_fixed;
    context->style.window.background = saved_background;
}
