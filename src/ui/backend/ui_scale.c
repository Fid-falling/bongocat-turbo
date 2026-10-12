/*
 * 缩放的三个来源, 别混用:
 *  - layout_scale: 逻辑缩放, 决定"一个 UI 单位等于几个窗口坐标", 输入坐标要除它;
 *  - raster_scale: 光栅缩放, 决定字体/图形按几倍像素烘焙;
 *  - 二者之比的另一半来自窗口的像素密度, 所以 query_window_scale 用
 *    display_scale / pixel_density 反推 layout_scale: 像素密度已经体现在窗口的
 *    物理尺寸里, 不除掉就等于把缩放算了两遍, 界面会整体偏大一圈。
 * BONGO_CAT_TEST_UI_SCALE 是给自动化测试固定缩放用的, 同时覆盖上面两者。
 */
#include "ui_backend.h"
/* 诊断行必须走 LIFECYCLE 类别: 落盘策略会丢掉普通 SDL_Log 的 INFO (见
   bongo_cat_log_enabled), 用它等于用户回传的日志里根本没有这一行。 */
#include "bongo_cat/log.h"

#include <math.h>
#include <stdlib.h>

#define BONGO_CAT_UI_SCALE_MIN 0.5f
#define BONGO_CAT_UI_SCALE_MAX 8.0f

static float valid_scale(float value) {
    if (!isfinite(value) || value < BONGO_CAT_UI_SCALE_MIN) return 1.0f;
    return SDL_min(value, BONGO_CAT_UI_SCALE_MAX);
}

static float test_scale(void) {
    const char *text = SDL_getenv("BONGO_CAT_TEST_UI_SCALE");
    if (!text || !text[0]) return 0.0f;
    char *end = NULL;
    float value = strtof(text, &end);
    return end && !*end && isfinite(value) &&
        value >= BONGO_CAT_UI_SCALE_MIN ?
        SDL_min(value, BONGO_CAT_UI_SCALE_MAX) : 0.0f;
}

float bongo_cat_ui_display_layout_scale(SDL_DisplayID display) {
    float override = test_scale();
    if (override > 0.0f) return override;
    return valid_scale(display ? SDL_GetDisplayContentScale(display) : 1.0f);
}

void bongo_cat_ui_query_window_scale(SDL_Window *window,
    float *layout_scale, float *raster_scale) {
    float override = test_scale();
    if (override > 0.0f) {
        if (layout_scale) *layout_scale = override;
        if (raster_scale) *raster_scale = override;
        return;
    }
    float raster = window ? SDL_GetWindowDisplayScale(window) : 1.0f;
    float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    raster = valid_scale(raster);
    density = valid_scale(density);
    if (layout_scale) *layout_scale = valid_scale(raster / density);
    if (raster_scale) *raster_scale = raster;
}

/*
 * 诊断一行, 用户回传日志时先看它:
 *   [ui] 设置窗口 逻辑=900x680 像素=1125x850 窗口=1125x850 缩放=1.25(布局)/1.25(光栅)
 * 逻辑与像素不相等就说明这台机器在缩放显示 (125% 即 1.25 倍)。逻辑尺寸是布局与输入
 * 坐标的空间, 像素尺寸是交换链/视口/裁剪矩形的空间, 两者混用正是"界面缺一块、大片
 * 空白、图标与开关消失"的成因 (见 ui_backend.c 的 render_vulkan)。窗口尺寸是 SDL 的
 * 窗口坐标, 与像素尺寸之比就是像素密度 —— 只看窗口尺寸分不清"窗口被按缩放建大了"
 * 还是"窗口没变、内容按缩放铺开"。只在建窗与缩放变化时调用, 不进每帧路径。
 */
void bongo_cat_ui_log_scale(const char *tag, SDL_Window *window) {
    int window_width = 0, window_height = 0;
    int pixel_width = 0, pixel_height = 0;
    if (window) {
        SDL_GetWindowSize(window, &window_width, &window_height);
        SDL_GetWindowSizeInPixels(window, &pixel_width, &pixel_height);
    }
    float layout_scale = 1.0f, raster_scale = 1.0f;
    bongo_cat_ui_query_window_scale(window, &layout_scale, &raster_scale);
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[ui] %s 逻辑=%.0fx%.0f 像素=%dx%d 窗口=%dx%d 缩放=%.2f(布局)/%.2f(光栅)",
        tag ? tag : "窗口", window_width / layout_scale,
        window_height / layout_scale, pixel_width, pixel_height,
        window_width, window_height, layout_scale, raster_scale);
}

void bongo_cat_ui_logical_size(const BongoCatUIBackend *ui,
    float *width, float *height) {
    int window_width = 1, window_height = 1;
    if (ui && ui->window)
        SDL_GetWindowSize(ui->window, &window_width, &window_height);
    float scale = ui && ui->layout_scale > 0.0f ? ui->layout_scale : 1.0f;
    if (width) *width = SDL_max(1.0f, window_width / scale);
    if (height) *height = SDL_max(1.0f, window_height / scale);
}
