/* 滚轮手势: 非 Ctrl 滚轮改缩放, Ctrl 滚轮改不透明度。不直接跳到目标值, 而是设置目标后
 * 由主循环每帧的 bongo_cat_window_update_wheel_animation 做指数逼近, 所以缩放是平滑的。
 * 手势期间几何一律以手势开始时的快照 (wheel_base_＊ / wheel_geometry_scale / wheel_center_＊)
 * 为基准, 免得连续滚动积累舍入误差。平台差异: Windows 靠 Raw Input 自己跟踪 Ctrl (SDL
 * 缓存的修饰键在失焦后会残留), 其它平台才合并 SDL_GetModState。 */
#include "runtime.h"
#include "window_wheel_internal.h"
#include "bongo_cat/preferences.h"

#define WHEEL_OPACITY_STEP 5.0f
#define WHEEL_SCALE_STEP 2.0f
#define WHEEL_SCALE_TARGET_LEAD (2.0f * WHEEL_SCALE_STEP)
#define WHEEL_OPACITY_RESPONSE_SECONDS 0.05f
#define WHEEL_SCALE_RESPONSE_SECONDS 0.025f
#define WHEEL_OPACITY_SPEED_PER_SECOND 100.0f
#define WHEEL_SCALE_SPEED_PER_SECOND 150.0f
#define WHEEL_MAX_FRAME_SECONDS 0.05f

/* SDL_MOUSEWHEEL_FLIPPED 是"自然滚动"方向, 取反后统一成"y > 0 表示滚轮向上";
 * event 可为 NULL (返回 0), 方向归一化只在这里做一次。 */
static float wheel_delta(const SDL_MouseWheelEvent *event) {
    float value = event ? event->y : 0.0f;
    return event && event->direction == SDL_MOUSEWHEEL_FLIPPED ? -value : value;
}

/* 过滤不属于本窗口的滚轮事件: 可见性、windowID、坐标落在窗口矩形内, 真事件还要复检当前
 * 桌面指针 (原因见函数内的行内注释)。返回 true 才允许后面的手势状态被改动。 */
static bool wheel_targets_window(BongoCatApp *app,
    const SDL_MouseWheelEvent *event) {
    int width = 0, height = 0;
    if (!app->session.window.visible ||
        event->windowID != SDL_GetWindowID(app->window) ||
        !SDL_GetWindowSize(app->window, &width, &height)) return false;
    if (event->mouse_x < 0.0f || event->mouse_x >= width ||
        event->mouse_y < 0.0f || event->mouse_y >= height) return false;
    /* A transparent/click-through window can occasionally retain mouse focus
       while a wheel message is generated elsewhere. SDL then reuses the last
       local wheel coordinates, so also verify the current desktop cursor for
       real events. Timestamp-zero events are synthetic self-test fixtures. */
    if (event->timestamp) {
        float screen_x = 0.0f, screen_y = 0.0f;
        float local_x = 0.0f, local_y = 0.0f;
        SDL_GetGlobalMouseState(&screen_x, &screen_y);
        if (!bongo_cat_platform_pointer_local(&app->platform, screen_x,
            screen_y, &local_x, &local_y) || local_x < 0.0f ||
            local_x >= width || local_y < 0.0f || local_y >= height)
            return false;
        if (!bongo_cat_window_visible_at_pointer(app, local_x, local_y))
            return false;
    }
    return true;
}

/* 手势基准快照: reset 为 false 表示沿用当前手势的基准 (直接返回 true)。记录窗口中心、
 * 基准尺寸与基准缩放, 并把两个动画时钟都对齐到当前时刻。
 * 拿不到窗口位置/尺寸或缩放非正时返回 false —— 调用方要放弃这次输入, 而不是用半套基准。 */
static bool initialize_targets(BongoCatApp *app, bool reset) {
    if (!reset) return true;
    int x = 0, y = 0, width = 0, height = 0;
    if (!SDL_GetWindowPosition(app->window, &x, &y) ||
        !SDL_GetWindowSize(app->window, &width, &height) ||
        width <= 0 || height <= 0 || app->session.window.scale_percent <= 0.0f)
        return false;
    app->wheel_opacity_target = app->session.window.opacity_percent;
    app->wheel_scale_target = app->session.window.scale_percent;
    app->wheel_center_x = x + width * 0.5f;
    app->wheel_center_y = y + height * 0.5f;
    app->wheel_geometry_scale = app->session.window.scale_percent;
    app->wheel_base_width = width;
    app->wheel_base_height = height;
    app->wheel_animation_ns = SDL_GetTicksNS();
    app->wheel_input_ns = app->wheel_animation_ns;
    return true;
}

int bongo_cat_window_wheel_round_position(float value) {
    return (int)(value + (value < 0.0f ? -0.5f : 0.5f));
}

/* 滚轮入口: 先过滤 (是否本窗口) 再归一手势, 然后只改目标值, 真正的平滑在 update 里。
 * 同一手势内 (与上一事件间隔 < GESTURE_IDLE_NS) 沿用旧基准, 否则重新采样窗口几何 ——
 * 基准必须固定, 否则连续滚动会积累舍入误差、窗口会漂。
 * 目标值没变 (已到上下限) 时提前返回, 但仍刷新 wheel_input_ns, 让动画知道手势还在继续。
 * 非 Ctrl 时缩放目标被夹在"当前缩放 ± WHEEL_SCALE_TARGET_LEAD"内, 快速滚动不会因动画滞后
 * 而冲过头; Ctrl 时改不透明度, 并先结束交互快照 (不透明度与快照预览是两套显示路径)。 */
void bongo_cat_window_wheel(BongoCatApp *app, const SDL_MouseWheelEvent *event) {
    if (!app || !app->window || !event ||
        !wheel_targets_window(app, event)) return;
    float delta = wheel_delta(event);
    if (SDL_fabsf(delta) < 0.001f) return;
    delta = SDL_clamp(delta, -1.0f, 1.0f);
    uint64_t event_ns = event->timestamp ? event->timestamp : SDL_GetTicksNS();
    bool continuing = app->wheel_gesture_active && event_ns >= app->wheel_event_ns &&
        event_ns - app->wheel_event_ns < BONGO_CAT_WHEEL_GESTURE_IDLE_NS;
    if (!initialize_targets(app, !continuing && !app->wheel_animation_active)) return;
    app->wheel_event_ns = event_ns;
    app->wheel_gesture_active = true;
    float old_opacity_target = app->wheel_opacity_target;
    float old_scale_target = app->wheel_scale_target;
    bool control = bongo_cat_input_control_down(&app->input);
#ifndef _WIN32
    control = control || (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
#endif
    /* Windows Raw Input tracks releases without keyboard focus; SDL's
       cached modifiers can retain Ctrl after it has been released. */
    if (!control) {
        float minimum = SDL_max(10.0f,
            app->session.window.scale_percent - WHEEL_SCALE_TARGET_LEAD);
        float maximum = SDL_min(500.0f,
            app->session.window.scale_percent + WHEEL_SCALE_TARGET_LEAD);
        app->wheel_scale_target = SDL_clamp(app->wheel_scale_target +
            delta * WHEEL_SCALE_STEP, minimum, maximum);
    } else {
        bongo_cat_window_snapshot_end(app);
        app->wheel_opacity_target = SDL_clamp(app->wheel_opacity_target +
            delta * WHEEL_OPACITY_STEP, 10.0f, 100.0f);
    }
    /* event_ns 用于判定手势是否连续, 动画一律用这里的 SDL_GetTicksNS 时钟 (两者不同源)。 */
    uint64_t input_ns = SDL_GetTicksNS();
    if (old_opacity_target == app->wheel_opacity_target &&
        old_scale_target == app->wheel_scale_target) {
        if (app->wheel_animation_active) app->wheel_input_ns = input_ns;
        return;
    }
    if (!app->wheel_animation_active) app->wheel_animation_ns = input_ns;
    app->wheel_input_ns = input_ns;
    app->wheel_animation_active = true;
    app->dirty = true;
}

/* 指数逼近 + 限速: 步长取"剩余距离 × dt/(response+dt)"(帧率无关的平滑) 再按
 * speed_per_second 限制 (大跳变不会瞬移); 距离小于 snap_distance 时直接贴到目标值,
 * 避免无限接近却永远到不了、动画永远不结束。 */
static float approach(float current, float target, float elapsed_seconds,
    float response_seconds, float speed_per_second, float snap_distance) {
    float difference = target - current;
    if (SDL_fabsf(difference) < snap_distance) return target;
    float alpha = elapsed_seconds / (response_seconds + elapsed_seconds);
    float step = difference * alpha;
    float maximum = speed_per_second * elapsed_seconds;
    step = SDL_clamp(step, -maximum, maximum);
    return SDL_fabsf(step) >= SDL_fabsf(difference) ? target : current + step;
}

/* 缩放动画的一步: 用 bongo_cat_window_scaled_size 换算实际可用缩放 (会被窗口尺寸上下限
 * 夹取), 夹取后的值必须回写 wheel_scale_target, 否则动画会一直追一个到不了的目标。
 * 尺寸没变且没有交互快照时只更新 session 的值、不重建表面; 应用失败则把目标值拉回失败前
 * 的实际缩放, 并结束动画与手势。 */
static void apply_scale(BongoCatApp *app, float scale) {
    float old = app->session.window.scale_percent;
    if (old <= 0.0f || SDL_fabsf(scale - old) < 0.001f) return;
    float actual;
    int next_width, next_height;
    if (!bongo_cat_window_scaled_size(app->wheel_base_width,
        app->wheel_base_height, app->wheel_geometry_scale,
        scale, &actual, &next_width, &next_height)) return;
    if (actual != scale) app->wheel_scale_target = actual;
    if (!app->window_snapshot && next_width == app->session.window.width &&
        next_height == app->session.window.height) {
        app->session.window.scale_percent = actual;
        return;
    }
    if (!bongo_cat_window_apply_scale_centered(app, actual,
        app->wheel_base_width, app->wheel_base_height,
        app->wheel_geometry_scale, app->wheel_center_x,
        app->wheel_center_y)) {
        app->wheel_scale_target = old;
        app->wheel_animation_active = false;
        app->wheel_gesture_active = false;
    }
}

/* 主循环每帧调一次 (app_loop.c), now 由调用方传入以便自测注入时间。帧间隔不足
 * BONGO_CAT_WHEEL_FRAME_INTERVAL_NS 直接返回 —— resize/expose 消息会提前唤醒主循环,
 * 不能让缩放跟着消息频率跑。单帧 dt 夹在 WHEEL_MAX_FRAME_SECONDS 内, 休眠或卡顿后不会
 * 一步跳到位。动画结束条件: 两个目标都到位, 且手势空闲超过 GESTURE_IDLE_NS。
 * dirty 只在真有像素变化时置位, 具体规则见下方行内注释。 */
void bongo_cat_window_update_wheel_animation(BongoCatApp *app, uint64_t now) {
    if (!app || !app->wheel_animation_active) return;
    uint64_t elapsed_ns = now >= app->wheel_animation_ns
        ? now - app->wheel_animation_ns : 0;
    /* Resize/expose messages can wake the loop before the next animation frame. */
    if (elapsed_ns < BONGO_CAT_WHEEL_FRAME_INTERVAL_NS) return;
    app->wheel_animation_ns = now;
    float elapsed_seconds = SDL_min((float)elapsed_ns / 1000000000.0f,
        WHEEL_MAX_FRAME_SECONDS);
    float opacity = approach(app->session.window.opacity_percent,
        app->wheel_opacity_target, elapsed_seconds,
        WHEEL_OPACITY_RESPONSE_SECONDS, WHEEL_OPACITY_SPEED_PER_SECOND, 0.01f);
    float scale = approach(app->session.window.scale_percent,
        app->wheel_scale_target, elapsed_seconds,
        WHEEL_SCALE_RESPONSE_SECONDS, WHEEL_SCALE_SPEED_PER_SECOND, 0.05f);
    bool changed = SDL_fabsf(opacity - app->session.window.opacity_percent) > 0.001f ||
        SDL_fabsf(scale - app->session.window.scale_percent) > 0.001f;
    bool opacity_changed =
        SDL_fabsf(opacity - app->session.window.opacity_percent) > 0.001f;
    if (opacity_changed) {
        app->session.window.opacity_percent = opacity;
        if (!app->hover_hidden) {
            bongo_cat_app_cancel_hover_fade(app);
            bongo_cat_platform_set_opacity(&app->platform, opacity / 100.0f);
        }
    }
    apply_scale(app, scale);
    bool reached =
        SDL_fabsf(app->session.window.opacity_percent - app->wheel_opacity_target) < 0.01f &&
        SDL_fabsf(app->session.window.scale_percent - app->wheel_scale_target) < 0.01f;
    bool recent_input = now >= app->wheel_input_ns &&
        now - app->wheel_input_ns < BONGO_CAT_WHEEL_GESTURE_IDLE_NS;
    app->wheel_animation_active = !reached || recent_input;
    /* Geometry marks actual pixel changes dirty; subpixel scale bookkeeping
       alone does not need another model draw and buffer swap. */
    if (opacity_changed || (!app->wheel_animation_active &&
        app->resize_render_target_pending)) app->dirty = true;
    if (changed) bongo_cat_preferences_invalidate(app->preferences);
}

/* 立刻停掉滚轮动画与手势标志 (菜单里选了确定的缩放值、几何操作要接管、退出时都会调)。
 * 只清标志: session 里的当前值保持不变, 不会被拉向 wheel_*_target。 */
void bongo_cat_window_cancel_wheel_animation(BongoCatApp *app) {
    if (!app) return;
    app->wheel_animation_active = false;
    app->wheel_gesture_active = false;
}
