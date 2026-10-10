/*
 * 段落标题用的大图标 (四枚: 窗口/模型/应用/外观)。
 * 与行图标不同, 这里的图形是按传入矩形的宽高自适应画的 (用 bounds.w/bounds.h 推算
 * 关键点), 因为段落头给图标的尺寸会随布局变; 行图标则是固定 18x18 网格。
 * 同样全部是矢量绘制, 理由见 preferences_row_icons.c 的文件头注释。
 */
#include "preferences_icons_internal.h"

static void window_icon(struct nk_command_buffer *canvas,
    struct nk_rect bounds, struct nk_color color) {
    nk_stroke_rect(canvas, nk_rect(bounds.x + 1, bounds.y + 2,
        bounds.w - 2, bounds.h - 4), 2, 1.5f, color);
    nk_stroke_line(canvas, bounds.x + 1, bounds.y + 6,
        bounds.x + bounds.w - 1, bounds.y + 6, 1.5f, color);
    nk_fill_circle(canvas, nk_rect(bounds.x + 3, bounds.y + 3, 2, 2), color);
    nk_fill_circle(canvas, nk_rect(bounds.x + 6, bounds.y + 3, 2, 2), color);
}

/*
 * model_icon 画的是一张"多面体/低模"轮廓: 8 条棱线拼出上下两个顶点, 正好对应"模型"
 * 这个语义; 所有顶点都从 bounds 推算, 所以拉宽压扁都不会散架。
 */
static void model_icon(struct nk_command_buffer *canvas,
    struct nk_rect bounds, struct nk_color color) {
    float left = bounds.x + 2, right = bounds.x + bounds.w - 2;
    float center = bounds.x + bounds.w * .5f;
    float top = bounds.y + 1, middle = bounds.y + 6;
    float joint = bounds.y + 9, bottom = bounds.y + bounds.h - 1;
    nk_stroke_line(canvas, center, top, left, middle, 1.5f, color);
    nk_stroke_line(canvas, center, top, right, middle, 1.5f, color);
    nk_stroke_line(canvas, left, middle, center, joint, 1.5f, color);
    nk_stroke_line(canvas, right, middle, center, joint, 1.5f, color);
    nk_stroke_line(canvas, left, middle, left, bounds.y + 13, 1.5f, color);
    nk_stroke_line(canvas, right, middle, right, bounds.y + 13, 1.5f, color);
    nk_stroke_line(canvas, left, bounds.y + 13, center, bottom, 1.5f, color);
    nk_stroke_line(canvas, right, bounds.y + 13, center, bottom, 1.5f, color);
    nk_stroke_line(canvas, center, joint, center, bottom, 1.5f, color);
}

static void application_icon(struct nk_command_buffer *canvas,
    struct nk_rect bounds, struct nk_color color) {
    nk_stroke_rect(canvas, nk_rect(bounds.x + 1, bounds.y + 1,
        bounds.w - 2, bounds.h - 2), 3, 1.5f, color);
    for (int row = 0; row < 2; ++row)
        for (int column = 0; column < 2; ++column)
            nk_fill_rect(canvas, nk_rect(bounds.x + 4 + column * 6,
                bounds.y + 4 + row * 6, 4, 4), 1, color);
}

static void appearance_icon(struct nk_command_buffer *canvas,
    struct nk_rect bounds, struct nk_color color) {
    float x = bounds.x + bounds.w * .5f, y = bounds.y + bounds.h * .5f;
    nk_stroke_circle(canvas, nk_rect(x - 4, y - 4, 8, 8), 1.5f, color);
    const float rays[][4] = {
        {0, -8, 0, -6}, {0, 6, 0, 8}, {-8, 0, -6, 0}, {6, 0, 8, 0},
        {-6, -6, -4.5f, -4.5f}, {4.5f, 4.5f, 6, 6},
        {4.5f, -4.5f, 6, -6}, {-6, 6, -4.5f, 4.5f}};
    for (int i = 0; i < 8; ++i)
        nk_stroke_line(canvas, x + rays[i][0], y + rays[i][1],
            x + rays[i][2], y + rays[i][3], 1.5f, color);
}

/*
 * 段落图标总表: 用 if/else 链而不是函数指针数组, 因为这几枚图标数量少、且这里要
 * 显式返回 false 告诉调用方"不是我管的图标"(统一入口靠这个返回值分派到行图标那边)。
 */
bool bongo_cat_pref_section_icon_draw(struct nk_command_buffer *canvas,
    struct nk_rect bounds, BongoCatPrefIcon icon,
    struct nk_color color) {
    if (icon == BONGO_CAT_PREF_ICON_SECTION_WINDOW)
        window_icon(canvas, bounds, color);
    else if (icon == BONGO_CAT_PREF_ICON_SECTION_MODEL)
        model_icon(canvas, bounds, color);
    else if (icon == BONGO_CAT_PREF_ICON_SECTION_APPLICATION)
        application_icon(canvas, bounds, color);
    else if (icon == BONGO_CAT_PREF_ICON_SECTION_APPEARANCE)
        appearance_icon(canvas, bounds, color);
    else return false;
    return true;
}
