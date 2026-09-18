#include "ui_catime.h"

void bongo_cat_ui_fallback_icon(struct nk_command_buffer *canvas,
    int index, struct nk_rect bounds, struct nk_color color) {
    float x = bounds.x + bounds.w * .5f, y = bounds.y + 17;
    if (index == 0) {
        nk_stroke_circle(canvas, nk_rect(x - 9, y - 7, 18, 16), 2, color);
        nk_stroke_triangle(canvas, x - 9, y - 3, x - 7, y - 11,
            x - 2, y - 6, 2, color);
        nk_stroke_triangle(canvas, x + 2, y - 6, x + 7, y - 11,
            x + 9, y - 3, 2, color);
    } else if (index == 1) {
        nk_stroke_circle(canvas, nk_rect(x - 9, y - 9, 18, 18), 5, color);
        nk_fill_circle(canvas, nk_rect(x - 3, y - 3, 6, 6), color);
    } else if (index == 2) {
        nk_stroke_line(canvas, x - 8, y + 8, x + 7, y - 7, 3, color);
        nk_stroke_line(canvas, x - 7, y - 7, x + 7, y + 7, 2, color);
        nk_fill_circle(canvas, nk_rect(x + 7, y - 11, 4, 4), color);
    } else if (index == 3) {
        nk_stroke_rect(canvas, nk_rect(x - 10, y - 7, 20, 14), 3, 2, color);
        for (int row = 0; row < 2; ++row)
            for (int col = 0; col < 4; ++col)
                nk_fill_rect(canvas, nk_rect(x - 7 + col * 4,
                    y - 4 + row * 5, 2, 2), 0, color);
    } else {
        nk_fill_circle(canvas, nk_rect(x - 9, y - 8, 11, 13), color);
        nk_fill_circle(canvas, nk_rect(x - 1, y - 8, 11, 13), color);
        nk_fill_triangle(canvas, x - 8, y - 1, x + 9, y - 1,
            x, y + 10, color);
    }
}
