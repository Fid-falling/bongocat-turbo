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

/* B: 不依赖窗口消息的拖动驱动。
   代理分层呈现 / 穿透模式下, 真实窗口收不到鼠标消息, 而拖动原先只由 SDL 的
   BUTTON_DOWN + MOTION 事件触发 —— 于是"开了透明就拖不动"。这里改成轮询全局鼠标:
   左键按下(上升沿)且光标落在模型不透明像素上就进入拖动候选, 之后每帧喂一次 motion。
   motion 自己读全局坐标, 事件只用于非空判断, 所以传一个零值事件即可。
   正常模式下触发条件与原来一致(同样是"按在不透明像素上"), 行为不变。 */
void bongo_cat_window_poll_global_drag(BongoCatApp *app) {
    static bool previous_down = false;
    if (!app || !app->window || !app->session.window.visible ||
        app->window_minimized || app->hover_hidden ||
        app->settings.window.pass_through) {
        previous_down = false;
        return;
    }
    float gx = 0.0f, gy = 0.0f;
    bool down = (SDL_GetGlobalMouseState(&gx, &gy) & SDL_BUTTON_LMASK) != 0;
    bool fresh_press = down && !previous_down;
    previous_down = down;
    if (app->drag_candidate || app->window_drag_active) {
        SDL_MouseMotionEvent event = {0};
        event.type = SDL_EVENT_MOUSE_MOTION;
        bongo_cat_window_drag_motion(app, &event);
        return;
    }
    if (!fresh_press) return;
    int window_x = 0, window_y = 0, width = 0, height = 0;
    if (!SDL_GetWindowPosition(app->window, &window_x, &window_y)) return;
    SDL_GetWindowSize(app->window, &width, &height);
    /* 无边框窗口的客户区原点就是窗口原点, 全局坐标减去它即为窗口内坐标。 */
    float local_x = gx - (float)window_x, local_y = gy - (float)window_y;
    /*
     * 必须先要求光标落在窗口矩形内。只靠下面的命中测试是不够的: 命中状态在"尚未算出"
     * 时是保守值, 单用它就会出现"在任何地方点击都拖动桌宠"。
     */
    if (local_x < 0.0f || local_y < 0.0f || local_x >= (float)width ||
        local_y >= (float)height) return;
    if (!bongo_cat_window_visible_at_pointer(app, local_x, local_y)) return;
    SDL_MouseButtonEvent event = {0};
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button = SDL_BUTTON_LEFT;
    event.down = true;
    event.clicks = 1;
    event.x = local_x;
    event.y = local_y;
    bongo_cat_window_drag_begin(app, &event);
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
