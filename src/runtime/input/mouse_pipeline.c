/*
 * 鼠标采样管道: 每轮从输入状态里取出一次指针位置, 判断这次采样是否新鲜, 再决定哪些坐标
 * 进模型、哪些只进窗口层。主循环 (app_input.c 的 drain_input) 每轮调用一次, 顺序在
 * update_hover 之前 —— 穿透与命中区的正确性依赖这个顺序。
 * 与其它模块的分工: 投影在这里解析一次并传给映射, 窗口命中/穿透由 shell/window_hit.c
 * 负责, 参数写法在 mouse_mapping.c。本文件只做"选哪个坐标"的决策。
 * 最容易改错的地方: Windows 下按键状态来自 Raw Input (mouse_button_event_pending),
 * 不能再用 SDL_GetGlobalMouseState 覆盖, 否则高速点击会丢按下事件。
 */
#include "runtime.h"
#include "mouse_internal.h"

#include <SDL3/SDL.h>

/* 无绝对坐标时用按钮轮询兜底 (非 Windows 路径)。标准按钮直接写参数, 侧键只记状态。
   返回 true 表示"按下状态真的变了", 调用方据此决定要不要下发坐标。 */
/* Select screen input, then map it independently for the model. */
#ifndef _WIN32
static bool reconcile_button(BongoCatApp *app, bool *current, bool pressed,
    const char *parameter) {
    if (*current == pressed) return false;
    *current = pressed;
    bongo_cat_live2d_set_parameter(app->live2d, parameter,
        pressed ? 1.0f : 0.0f);
    /* 松开时要让命中区失效: 一次点击结束后必须重算"这个像素是否可点", 否则下一次按下
       可能落在过期的透明状态上。 */
    if (!pressed) bongo_cat_window_mark_hit_dirty(app);
    app->dirty = true;
    return true;
}
#endif

/* 切换绝对/相对指针模式。相对模式只在"指针被系统/游戏锁定"时启用, 两个切换方向要分别
   处理: 进相对模式要复位位移基准, 退回绝对模式要释放平台层的相对采样资源。
   返回值表示模式 (或锁状态) 有没有变, 变了就必须让这一帧继续往下走。 */
static bool update_pointer_mode(BongoCatApp *app, bool cursor_locked) {
    bool relative = !app->settings.model.ignore_mouse && cursor_locked;
    bool changed = relative != app->pointer_relative_active ||
        (relative && cursor_locked != app->pointer_cursor_locked);
    if (changed) {
        /* 模式切换时丢弃 mver 指针的累计位置: 相对模式累加的是位移, 带着旧状态进来会从
           错误的位置起步。 */
        if (relative != app->pointer_relative_active)
            app->mver_pointer = (BongoCatMverPointerState){0};
        if (relative) bongo_cat_platform_relative_pointer_reset(&app->platform);
        else bongo_cat_platform_relative_pointer_release(&app->platform);
        app->pointer_relative_active = relative;
    }
    app->pointer_cursor_locked = cursor_locked;
    return changed;
}

/* 每轮调用一次: 取出指针采样, 决定是否刷新窗口层, 再把坐标交给模型。
   副作用: 会改 hover 状态、穿透状态、按钮参数, 并在有变化时置 app->dirty。 */
void bongo_cat_app_apply_mouse(BongoCatApp *app) {
    if (!app) return;
    double target_x, target_y;
    /* 采样时间戳一次取出并复用: 中途再取一次会让"新鲜度"判定与实际使用的坐标不属于
       同一时刻。0 说明没有采样过, 置 1 表示"有过采样" (0 被用作无效值)。 */
    bool received = bongo_cat_input_take_mouse(&app->input, &target_x, &target_y);
    uint64_t now = SDL_GetTicksNS();
    if (received) {
        app->mouse_last_ns = now ? now : 1;
    }
    float global_x = 0.0f, global_y = 0.0f;
    /* Windows 上按键状态由 Raw Input 维护 (mouse_button_event_pending), 这里的位置轮询
       不能改写按下状态; 非 Windows 才需要轮询补上。 */
    SDL_MouseButtonFlags buttons = SDL_GetGlobalMouseState(&global_x, &global_y);
    bool button_event_pending = app->mouse_button_event_pending;
    bool cursor_locked = bongo_cat_platform_pointer_locked(&app->platform);
    bool cursor_lock_changed = cursor_locked != app->pointer_cursor_locked;
    bool button_changed = false;
#ifdef _WIN32
    /* Model buttons belong to Raw Input; cursor polling is position-only. */
    (void)buttons;
#else
    /* 只有在没有原始按键事件、也没有锁指针时才轮询: 锁指针时鼠标被系统接管 (常被藏起来),
       轮询到的状态不能代表用户意图, 用错会把模型的手切成抬起。 */
    if (!button_event_pending && !cursor_locked) {
        button_changed = reconcile_button(app, &app->left_mouse_down,
            (buttons & SDL_BUTTON_LMASK) != 0, "ParamMouseLeftDown");
        button_changed = reconcile_button(app, &app->right_mouse_down,
            (buttons & SDL_BUTTON_RMASK) != 0, "ParamMouseRightDown") ||
            button_changed;
        app->back_mouse_down = (buttons & SDL_BUTTON_X1MASK) != 0;
        app->forward_mouse_down = (buttons & SDL_BUTTON_X2MASK) != 0;
        bool side_down = app->back_mouse_down || app->forward_mouse_down;
        if (app->side_mouse_down != side_down) {
            app->side_mouse_down = side_down;
            button_changed = true;
            app->dirty = true;
        }
    }
#endif
    /* native_selected 决定用平台输入链路送来的位置, 还是退回 SDL 的全局鼠标查询。 */
    bool native_selected = received || (app->pointer_known &&
        bongo_cat_mouse_sample_fresh(now, app->mouse_last_ns));
    /* Native screen updates are optional; SDL supplies the current position. */
    if (!native_selected) {
        target_x = global_x;
        target_y = global_y;
    } else if (!received) {
        target_x = app->pointer_x;
        target_y = app->pointer_y;
    }
    /* Seed relative motion from the observed lock point. */
    /* 刚锁上指针的那一帧单独处理: 相对模式累加的是"自复位以来的位移", 必须先拿锁定时
       观察到的绝对坐标当基准, 否则会从 (0,0) 开始累加。 */
    if (cursor_locked && cursor_lock_changed) {
        target_x = global_x;
        target_y = global_y;
    }
    bool moved = !app->pointer_known || app->pointer_x != target_x ||
        app->pointer_y != target_y;
    if (moved) {
        bongo_cat_app_track_hover(app, target_x, target_y);
        /* Refresh the hit pixel before the next button press. Waiting for the
           scheduled frame can leave a stale transparent state for fast clicks. */
    /* 刷新命中的像素必须赶在下一次按下之前: 等调度到的那一帧可能已经太晚, 快速点击会
       落在过期的状态上。按住鼠标期间不采样 (拖拽/缩放期间命中判定会抖动)。 */
        if (app->click_through_applied && !app->left_mouse_down &&
            !app->right_mouse_down)
            bongo_cat_window_capture_pointer_hit(app);
    }
    bongo_cat_window_sync_click_through(app);
    bool pointer_mode_changed = update_pointer_mode(app, cursor_locked);
    app->mouse_button_event_pending = false;
    /* 按钮事件标记是"本函数消费一次"的: 必须在早退分支之前清掉, 否则一次 Raw Input 按下
       会让后面每一轮都以为还有待处理的按钮事件。 */
    if (app->settings.model.ignore_mouse) return;
    /* 只更新窗口层 (悬停/穿透/按钮), 不碰模型: 用户开了"忽略鼠标"时模型完全静止,
       但窗口该消失还是要消失。 */
    bool relative_requested = app->pointer_relative_active;
    /* 没有任何输入变化时的早退。注意顺序: 按钮边沿、指针模式变更与待处理的按钮事件也可能
       置脏, 不能被这一句吞掉。 */
    if (!relative_requested && !moved && !button_changed &&
        !pointer_mode_changed && !button_event_pending) return;

    /* Resolve once: tracking limits and model output share the same display,
       custom workarea and centered anchor for this update. */
    /* 投影只解析一次: 跟踪限制与模型输出必须共用同一套基准 (多显示器下解析两次会得到
       不同结果, 表现为手指与模型基准不一致)。投影失败表示这一帧指针位置不可信。 */
    BongoCatMouseProjection projection;
    if (!bongo_cat_app_mouse_projection(app, target_x, target_y, &projection))
        return;
    double model_x = target_x, model_y = target_y;
    bool model_moved = moved;
    bool profile_relative = app->model_render_options.mver_projection &&
        app->model_render_options.mouse_force_move;
    /* 光标的绝对位置被锁住时, 模型坐标才是唯一可信的指针坐标; 锁定时 gaze 也用模型坐标,
       因为相对模式下它是累计值, 与指针实际位置不是一回事。 */
    bool map_pointer = cursor_locked || (app->model_render_options.mver_projection &&
        (!app->settings.model.mouse_centered || profile_relative));
    if (map_pointer && !bongo_cat_app_map_pointer(app, &projection,
            relative_requested, target_x, target_y, &model_x, &model_y,
            &model_moved)) {
    /* 映射失败就退回原始屏幕坐标: 语义等同于"没做映射", 而不是"指针在 (0,0)"。 */
        model_x = target_x; model_y = target_y; model_moved = moved;
    }
    /* 没有移动、没有按键状态变化、也没有按钮事件时不下发: 空闲时不重复写同一批参数。 */

    if (model_moved || button_changed || pointer_mode_changed || button_event_pending)
        bongo_cat_app_apply_mouse_coordinates(app, &projection, model_x, model_y,
            cursor_locked ? model_x : target_x, cursor_locked ? model_y : target_y);
}
