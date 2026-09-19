#include "runtime.h"
#include "window_wheel_internal.h"
#include "bongo_cat/preferences.h"
#include "bongo_cat/vk_backend.h"
#include <stdlib.h>

static uint64_t frame_interval_ns(const BongoCatApp *app) {
#ifdef BONGO_CAT_HAS_CUBISM
    int fps = app ? app->settings.model.max_fps : BONGO_CAT_DEFAULT_MAX_FPS;
    /* 无限制: 间隔为 0 -> 每轮循环都判定为「该出新帧」, 不再做节流 */
    if (fps == BONGO_CAT_MAX_FPS_UNLIMITED) return 0;
    if (fps <= 0) fps = BONGO_CAT_DEFAULT_MAX_FPS;
    return 1000000000ull / (uint64_t)fps;
#else
    (void)app;
    /* 诊断后端没有真实模型动画, 保持固定的 10 FPS 推进节奏 */
    return 100000000ull;
#endif
}

static int remaining_ms(uint64_t deadline, uint64_t now) {
    if (deadline <= now) return 0;
    uint64_t ns = deadline - now;
    uint64_t ms = ns / 1000000ull + (ns % 1000000ull != 0);
    return ms > 250 ? 250 : (int)ms;
}

bool bongo_cat_model_frame_due(const BongoCatApp *app, uint64_t now) {
    return app && app->session.window.visible && !app->window_minimized &&
        (!app->last_frame_ns || now >= app->last_frame_ns +
            frame_interval_ns(app));
}

/*
 * 把"最大帧率"设置下发给后端的呈现路径。
 *
 * 界面里的三档是 30 / 60 / 无限 (BONGO_CAT_MAX_FPS_UNLIMITED)。之前两个后端都
 * 无条件开着垂直同步: 选"无限"也被锁在显示器刷新率上 (100 Hz 屏 = 100 FPS),
 * 所以这里在"无限"时关掉同步, 30/60 时保持同步 (与 OpenGL 路径原来的行为一致)。
 *
 * 帧率上限本身由 frame_interval_ns() 决定, 这个函数只负责呈现侧的同步策略。
 * 设置变化时 Vulkan 会重建交换链 (呈现模式是创建时定下的), 所以只在值变化时下发。
 */
void bongo_cat_app_apply_frame_pacing(BongoCatApp *app) {
    if (!app) return;
    int fps = app->settings.model.max_fps;
    if (fps == app->applied_max_fps) return;
    bool vsync = fps != BONGO_CAT_MAX_FPS_UNLIMITED;
    app->applied_max_fps = fps;
    if (app->vk) {
        bongo_cat_vk_set_vsync(app->vk, vsync);
        return;
    }
    if (!app->window || !app->gl_context) return;
    if (!SDL_GL_MakeCurrent(app->window, app->gl_context)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "Frame pacing could not activate the main GL context: %s",
            SDL_GetError());
        return;
    }
    if (!SDL_GL_SetSwapInterval(vsync ? 1 : 0))
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "Frame pacing could not set vertical sync (%d): %s", vsync ? 1 : 0,
            SDL_GetError());
    SDL_Log("[runtime] 垂直同步已%s (最大帧率=%d)", vsync ? "开启" : "关闭", fps);
}

int bongo_cat_window_wait_timeout(const BongoCatApp *app, uint64_t now) {
    if (!app) return 250;
    uint64_t frame_deadline = app->last_frame_ns + frame_interval_ns(app);
    int wait_ms = app->session.window.visible && !app->window_minimized ? remaining_ms(
        frame_deadline, now) : 250;
    if (bongo_cat_preferences_needs_frame(app->preferences) && wait_ms > 4)
        wait_ms = 4;
    if (app->wheel_animation_active) {
        int wheel_wait = remaining_ms(app->wheel_animation_ns +
            BONGO_CAT_WHEEL_FRAME_INTERVAL_NS, now);
        if (wait_ms > wheel_wait) wait_ms = wheel_wait;
    }
    if (app->hover_fade_active && wait_ms > 8) wait_ms = 8;
    if (app->window_snapshot && wait_ms > 16) wait_ms = 16;
    if (app->session.window.visible && !app->window_minimized &&
        (app->click_through_applied || (app->settings.window.pass_through &&
            app->settings.window.always_on_top && app->settings.window.hide_on_hover)) &&
        wait_ms > 16) wait_ms = 16;
    bool pending_hit = app->session.window.visible && !app->window_minimized &&
        app->pointer_hit_dirty &&
        app->pointer_hit_deadline_ns && !app->settings.window.pass_through &&
        !app->hover_hidden && !app->left_mouse_down && !app->right_mouse_down;
    if (!pending_hit) return wait_ms;
    int hit_wait = remaining_ms(app->pointer_hit_deadline_ns, now);
    return hit_wait < wait_ms ? hit_wait : wait_ms;
}

bool bongo_cat_wait_event(SDL_Event *event, int timeout_ms) {
    if (!event) return false;
    if (timeout_ms <= 0) return SDL_PollEvent(event);
    uint64_t deadline = SDL_GetTicksNS() + (uint64_t)timeout_ms * 1000000ull;
    for (;;) {
        int remaining = remaining_ms(deadline, SDL_GetTicksNS());
        if (!remaining) return false;
        if (SDL_WaitEventTimeout(event, remaining)) return true;
        uint64_t now = SDL_GetTicksNS();
        if (now >= deadline) return false;
        SDL_Delay(1);
    }
}

bool bongo_cat_window_wait_timeout_self_test(void) {
    const uint64_t now = 1000000000ull;
    BongoCatApp *app = calloc(1, sizeof(*app));
    if (!app) return false;
    bool passed = false;
    app->session.window.visible = true;
    app->settings.model.max_fps = 60;
    app->last_frame_ns = now;
#ifdef BONGO_CAT_HAS_CUBISM
    app->settings.model.max_fps = 30;
    uint64_t interval_30 = frame_interval_ns(app);
    app->settings.model.max_fps = 60;
    uint64_t interval_60 = frame_interval_ns(app);
    app->settings.model.max_fps = 120;
    uint64_t interval_120 = frame_interval_ns(app);
    if (!(interval_30 > interval_60 && interval_60 > interval_120) ||
        bongo_cat_model_frame_due(app, now + interval_120 - 1) ||
        !bongo_cat_model_frame_due(app, now + interval_120)) goto done;
    app->settings.model.max_fps = BONGO_CAT_MAX_FPS_UNLIMITED;
    if (frame_interval_ns(app) != 0 ||
        !bongo_cat_model_frame_due(app, now) ||
        bongo_cat_window_wait_timeout(app, now) != 0) goto done;
    app->settings.model.max_fps = 60;
#endif
    int frame_wait = remaining_ms(now + frame_interval_ns(app), now);
    if (bongo_cat_window_wait_timeout(app, now) != frame_wait ||
        bongo_cat_model_frame_due(app, now)) goto done;
    app->pointer_hit_dirty = true;
    app->pointer_hit_deadline_ns = now + 8000000ull;
    if (bongo_cat_window_wait_timeout(app, now) != 8) goto done;
    app->pointer_hit_deadline_ns = now;
    if (bongo_cat_window_wait_timeout(app, now) != 0) goto done;
    app->pointer_hit_dirty = false;
#ifdef BONGO_CAT_HAS_CUBISM
    app->settings.model.max_fps = 1;
#endif
    app->click_through_applied = true;
    if (bongo_cat_window_wait_timeout(app, now) != 16) goto done;
    app->click_through_applied = false;
    app->settings.window.pass_through = true;
    app->settings.window.always_on_top = true;
    app->settings.window.hide_on_hover = true;
    if (bongo_cat_window_wait_timeout(app, now) != 16) goto done;
    app->settings.window.always_on_top = false;
    if (bongo_cat_window_wait_timeout(app, now) !=
        remaining_ms(now + frame_interval_ns(app), now)) goto done;
    app->settings.window.pass_through = false;
    app->settings.window.hide_on_hover = false;
#ifdef BONGO_CAT_HAS_CUBISM
    app->settings.model.max_fps = 60;
#endif
    if (!bongo_cat_model_frame_due(app,
        now + frame_interval_ns(app))) goto done;
    app->window_minimized = true;
    if (bongo_cat_model_frame_due(app, now + frame_interval_ns(app)) ||
        bongo_cat_window_wait_timeout(app, now) != 250) goto done;
    app->window_minimized = false;
    app->session.window.visible = false;
    if (bongo_cat_window_wait_timeout(app, now) != 250) goto done;
    app->wheel_animation_active = true;
    app->wheel_animation_ns = now;
    if (bongo_cat_window_wait_timeout(app, now) != 8 ||
        bongo_cat_window_wait_timeout(app, now + 3000000ull) != 5 ||
        bongo_cat_window_wait_timeout(app, now + 8000000ull) != 0) goto done;
    passed = true;
done:
    free(app);
    return passed;
}
