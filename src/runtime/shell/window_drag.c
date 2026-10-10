/* 拖动窗口: 左键按在"可见像素"上成为候选, 位移超过阈值才真正开始拖动。Windows 一律自己
 * 跟指针移动 (原生标题栏拖动会把窗口顶部硬拉回屏幕内, 即使关了 keep_in_screen); 其它平台
 * 只在 keep_in_screen 打开时这样, 否则交给平台的原生拖动。
 * 落点统一走 bongo_cat_window_drag_to, 由它负责越界夹取与预览几何同步。
 */
#include "runtime.h"
#include "modal_frame.h"

static int rounded_delta(float value) {
    return value < 0.0f ? (int)(value - 0.5f) : (int)(value + 0.5f);
}

/* 返回 true 表示由本模块按指针位移移动窗口, 而不是让平台做原生拖动; Windows 恒为 true。 */
static bool use_pointer_drag(const BongoCatApp *app) {
#ifdef _WIN32
    /* Native caption dragging pulls partially offscreen windows back down
       at the top edge, even when keep-in-screen is disabled. */
    (void)app;
    return true;
#else
    return app->settings.window.keep_in_screen;
#endif
}

/* 按"按下时的窗口原点 + 指针总位移"算落点; 越界与否交给 drag_to 判断。 */
static void move_with_pointer(BongoCatApp *app, float x, float y) {
    int next_x = app->drag_window_x + rounded_delta(x - app->drag_start_x);
    int next_y = app->drag_window_y + rounded_delta(y - app->drag_start_y);
    bongo_cat_window_drag_to(app, next_x, next_y);
}

/* 只有按在模型不透明的像素上才算拖动候选 (透明区域留给桌面点击); 同时记下窗口原点与按下
   时的全局指针位置, 作为整段拖动的基准。 */
void bongo_cat_window_drag_begin(BongoCatApp *app,
    const SDL_MouseButtonEvent *event) {
    if (!app || !event) return;
    bongo_cat_window_cancel_wheel_animation(app);
    app->drag_candidate = bongo_cat_window_visible_at_pointer(
        app, event->x, event->y);
    if (!app->drag_candidate || !SDL_GetWindowPosition(app->window,
        &app->drag_window_x, &app->drag_window_y)) {
        app->drag_candidate = false; return;
    }
    SDL_GetGlobalMouseState(&app->drag_start_x, &app->drag_start_y);
}

/* 候选态先看位移够不够大 (x² + y² >= 9, 约 3px) 才升级成真拖动, 避免点击时窗口抖动; 升级
   那一次才建交互快照、捕获鼠标。全局左键已经松开就直接结束 —— 松开事件可能发生在窗口
   之外, 不能只等 BUTTON_UP。 */
void bongo_cat_window_drag_motion(BongoCatApp *app,
    const SDL_MouseMotionEvent *event) {
    if (!app || !event || (!app->drag_candidate && !app->window_drag_active)) return;
    float pointer_x = 0.0f, pointer_y = 0.0f;
    SDL_MouseButtonFlags buttons = SDL_GetGlobalMouseState(&pointer_x, &pointer_y);
    if (!(buttons & SDL_BUTTON_LMASK)) {
        bongo_cat_window_drag_end(app); return;
    }
    if (app->window_drag_active) {
        if (use_pointer_drag(app))
            move_with_pointer(app, pointer_x, pointer_y);
        return;
    }
    float x = pointer_x - app->drag_start_x;
    float y = pointer_y - app->drag_start_y;
    if (x * x + y * y < 9.0f) return;
    app->drag_candidate = false;
    app->window_drag_active = true;
    bongo_cat_window_snapshot_begin(app);
    if (use_pointer_drag(app)) {
        if (app->settings.window.keep_in_screen)
            bongo_cat_window_drag_bounds_refresh(app);
        if (!SDL_CaptureMouse(true)) SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "Mouse capture is unavailable during window drag: %s",
            SDL_GetError());
        move_with_pointer(app, pointer_x, pointer_y);
    } else {
        BongoCatModalFrame modal_frame;
        bongo_cat_modal_frame_init(&modal_frame, app);
        bongo_cat_platform_begin_drag(&app->platform,
            bongo_cat_modal_frame_tick, &modal_frame);
    }
}

/* 幂等, 未在拖动时调用也安全 (隐藏、最小化、退出路径都会调)。结束前先夹回显示器, 并丢弃
   显示器布局缓存; 最后把指针位置重新喂给模型, mouse_centered 下它要跟着窗口走。 */
void bongo_cat_window_drag_end(BongoCatApp *app) {
    if (!app) return;
    bool was_active = app->window_drag_active;
    if (was_active) SDL_CaptureMouse(false);
    app->drag_candidate = false;
    bongo_cat_window_clamp_to_display(app);
    app->window_drag_active = false;
    bongo_cat_window_snapshot_end(app);
    bongo_cat_window_drag_bounds_clear(app);
    if (was_active && app->settings.model.mouse_centered) {
        float pointer_x = 0.0f, pointer_y = 0.0f;
        SDL_GetGlobalMouseState(&pointer_x, &pointer_y);
        bongo_cat_app_apply_mouse_position(app, pointer_x, pointer_y, 0.0f);
    }
}
