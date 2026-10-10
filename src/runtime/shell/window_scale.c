/* 以"窗口中心不动"的方式改缩放: 滚轮缩放与设置页的缩放滑杆都走这里, 保证放大缩小时视觉
 * 中心 (桌宠站的位置) 不跑。scale 是百分比, 基准状态由调用方给 —— 滚轮手势传的是手势
 * 开始时的尺寸与缩放快照, 免得连续滚动积累舍入误差。
 * 有交互快照时同步预览矩形; 非 Windows 平台快照是空实现, 但窗口状态照常维护。
 */
#include "runtime.h"

/* 屏幕坐标可以是负数, 所以按符号决定取整偏移 (int 截断朝零, 否则负半像素会偏)。 */
static int round_position(double value) {
    return (int)(value + (value < 0.0 ? -0.5 : 0.5));
}

/* 以当前窗口中心为基准改缩放: 用"当前尺寸 + 当前缩放"作基准, 因为 session 里的尺寸就是
   真实窗口尺寸。返回 false 表示没有变化 (已经在目标值上) 或窗口不可用 —— 不是错误, 调用
   方不必报错。 */
bool bongo_cat_window_set_scale(BongoCatApp *app, float scale) {
    if (!app || !app->window) return false;
    int x, y, width, height;
    if (!SDL_GetWindowPosition(app->window, &x, &y) ||
        !SDL_GetWindowSize(app->window, &width, &height)) return false;
    float actual;
    int next_width, next_height;
    if (!bongo_cat_window_scaled_size(width, height,
        app->session.window.scale_percent, scale,
        &actual, &next_width, &next_height)) return false;
    if (actual == app->session.window.scale_percent &&
        next_width == width && next_height == height) return false;
    return bongo_cat_window_apply_scale_centered(app, scale,
        width, height, app->session.window.scale_percent,
        x + width * 0.5f, y + height * 0.5f);
}

/* scale 是百分比; base_* 与 base_scale 是基准状态 (通常是手势开始时的快照), center_* 是
   屏幕坐标的中心点。有交互快照时同步子像素预览矩形, 滚轮缩放的平滑就来自这里。尺寸真的
   变了才重建表面, 并在返回前补画一帧 —— 否则合成器可能先显示"移动后的旧帧", 看起来是
   跳动。 */
bool bongo_cat_window_apply_scale_centered(BongoCatApp *app, float scale,
    int base_width, int base_height, float base_scale,
    float center_x, float center_y) {
    float actual;
    int width, height;
    if (!app || !app->window || !bongo_cat_window_scaled_size(base_width, base_height,
        base_scale, scale, &actual, &width, &height)) return false;
    int x = round_position(center_x - width * 0.5);
    int y = round_position(center_y - height * 0.5);
    SDL_FRect image = {0, 0, base_width * actual / base_scale,
        base_height * actual / base_scale};
    image.x = center_x - image.w * 0.5f;
    image.y = center_y - image.h * 0.5f;
    if (app->settings.window.keep_in_screen) {
        SDL_DisplayID display = SDL_GetDisplayForWindow(app->window);
        SDL_Rect bounds;
        if (display && SDL_GetDisplayUsableBounds(display, &bounds)) {
            x = SDL_clamp(x, bounds.x, SDL_max(bounds.x, bounds.x + bounds.w - width));
            y = SDL_clamp(y, bounds.y, SDL_max(bounds.y, bounds.y + bounds.h - height));
            image.x = SDL_clamp(image.x, (float)bounds.x,
                SDL_max((float)bounds.x, bounds.x + bounds.w - image.w));
            image.y = SDL_clamp(image.y, (float)bounds.y,
                SDL_max((float)bounds.y, bounds.y + bounds.h - image.h));
        }
    }
    if (x == app->session.window.x && y == app->session.window.y &&
        width == app->session.window.width && height == app->session.window.height) {
        app->session.window.scale_percent = actual;
        if (app->window_snapshot) {
            bongo_cat_window_snapshot_geometry(app, &image);
            bongo_cat_window_snapshot_present(app);
        }
        return true;
    }
    bongo_cat_window_snapshot_begin(app);
    bongo_cat_window_snapshot_geometry(app, &image);
    if (!bongo_cat_window_apply_geometry(app, x, y, actual, width, height)) {
        bongo_cat_window_snapshot_end(app);
        return false;
    }
    /* Uniform scaling preserves the normalized gaze anchor. Finish drawing
       the resized surface before returning to event/model processing, where
       the compositor could otherwise display the moved previous frame. */
    app->resize_render_target_pending = true;
    SDL_Window *previous_window = SDL_GL_GetCurrentWindow();
    SDL_GLContext previous_context = SDL_GL_GetCurrentContext();
    bongo_cat_app_render_now(app);
    if (previous_window && previous_context &&
        (previous_window != app->window || previous_context != app->gl_context) &&
        !SDL_GL_MakeCurrent(previous_window, previous_context))
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
            "Cannot restore OpenGL context after window scaling: %s", SDL_GetError());
    return true;
}
