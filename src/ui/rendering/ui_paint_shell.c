/*
 * 圆角外壳 (窗口底板) 的绘制。声明在 ui_paint.h 里, 实现拆到本文件只是为了不让
 * ui_paint.c 继续膨胀 (调用方不需要知道它在哪个 .c)。
 *
 * 与 ui_paint.c 里那几个效果的差别: 这个形状只有覆盖率、没有颜色, 所以走
 * 单通道 alpha 纹理, 颜色整个由 draw 时的 tint 给 —— 主题切换只要换 tint,
 * 不必重新烘焙, 缓存命中率因此高得多。
 * 另一个不变量: 缩放倍率上限 2.25 与 ui_paint.c 保持一致, 否则同一块边界上
 * 两个缓存项的圆角半径会算出不同像素值, 接缝处能看出错位。
 */
#include "ui_paint.h"
#include "ui_backend.h"
#include "ui_paint_cache.h"

#include <math.h>
#include <stdlib.h>

#define BONGO_CAT_UI_SHELL_SCALE_MAX 2.25f

static float rounded_distance(float x, float y, float width, float height,
    float radius) {
    radius = NK_CLAMP(0.0f, radius, NK_MIN(width, height) * .5f);
    float qx = fabsf(x - width * .5f) - (width * .5f - radius);
    float qy = fabsf(y - height * .5f) - (height * .5f - radius);
    float ox = NK_MAX(qx, 0.0f), oy = NK_MAX(qy, 0.0f);
    return sqrtf(ox * ox + oy * oy) +
        NK_MIN(NK_MAX(qx, qy), 0.0f) - radius;
}

void bongo_cat_ui_paint_rounded_surface(struct nk_context *context,
    struct nk_rect bounds, float rounding, struct nk_color color) {
    BongoCatUIBackend *backend = bongo_cat_ui_backend_for_context(context);
    if (!backend || !backend->window) {
        nk_fill_rect(nk_window_get_canvas(context), bounds, rounding, color);
        return;
    }
    float logical_width = 0.0f, logical_height = 0.0f;
    int pixel_width = 0, pixel_height = 0;
    bongo_cat_ui_logical_size(backend, &logical_width, &logical_height);
    SDL_GetWindowSizeInPixels(backend->window, &pixel_width, &pixel_height);
    if (logical_width < 1 || logical_height < 1) return;
    float sx = NK_MIN(pixel_width / logical_width,
        BONGO_CAT_UI_SHELL_SCALE_MAX);
    float sy = NK_MIN(pixel_height / logical_height,
        BONGO_CAT_UI_SHELL_SCALE_MAX);
    int width = NK_MAX(1, (int)ceilf(bounds.w * sx));
    int height = NK_MAX(1, (int)ceilf(bounds.h * sy));
    float raster_scale = NK_MIN(sx, sy);
    int radius = (int)lroundf(rounding * raster_scale);
    BongoCatUIPaintKey key = {BONGO_CAT_UI_PAINT_ROUNDED_SURFACE,
        width, height, radius, 0, 0, 0, 0};
    BongoCatUIPaintTexture *item =
        bongo_cat_ui_paint_cache_get(backend, &key);
    if (!item) return;
    if (!bongo_cat_ui_paint_cache_ready(item)) {
        unsigned char *pixels = malloc((size_t)width * height);
        if (!pixels) return;
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            float distance = rounded_distance(x + .5f, y + .5f,
                (float)width, (float)height, (float)radius);
            float coverage = NK_CLAMP(0.0f, .5f - distance, 1.0f);
            pixels[(size_t)y * width + x] =
                (unsigned char)(coverage * 255.0f + .5f);
        }
        if (!bongo_cat_ui_paint_cache_upload(item, pixels, true)) {
            free(pixels); return;
        }
        free(pixels);
    }
    bongo_cat_ui_paint_cache_draw(context, bounds, item, color);
}
