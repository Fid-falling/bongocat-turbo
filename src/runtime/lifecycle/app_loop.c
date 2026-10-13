/*
 * 主循环。一轮的顺序是有意的, 大致四段:
 *   1. 事件: 更新提示 -> 模型热更新 -> 设置窗口 -> 主窗口 依次有机会消费, 主窗口
 *      都没消费掉才会因 QUIT 结束循环;
 *   2. 周期性更新: 滚轮/快照/多宠/随机表情/音频/显示器恢复/运行时流程/输入;
 *   3. 模型推进: 只有"该出帧"时才 step_live2d (帧预算见 frame_clock.c); 窗口不可见
 *      或最小化时改为消费一次 idle 内存回收 (memory_policy);
 *   4. 渲染与收尾: 渲染可能发生两次 (设置窗口会改状态), 最后同步托盘、置前窗口、
 *      落盘配置。
 * 不要在这些渲染点之外随手加渲染: app->dirty 的语义与 memory_policy 的"延迟两帧"
 * 都假设渲染只发生在这些地方。
 */
#include "runtime.h"
#include "bongo_cat/audio.h"
#include "bongo_cat/log.h"
#include "model_cover.h"
#include "bongo_cat/overlay.h"
#include "bongo_cat/preferences.h"
#include "bongo_cat/render.h"
#include "bongo_cat/tray.h"

#include <SDL3/SDL_opengl.h>

/* 事件分发优先级 = 谁先拿到谁处理: 未消费的事件才轮到主窗口。返回 false 表示主窗口
   收到 QUIT (或窗口层要求退出), 由上层结束循环。 */
static void handle_event(BongoCatApp *app, const SDL_Event *event) {
    if (bongo_cat_update_event(app->update, event)) return;
    if (bongo_cat_model_refresh_event(app, event)) return;
    if (bongo_cat_preferences_event(app->preferences, event)) return;
    if (!bongo_cat_window_event(app, event)) app->running = false;
    if (event->type >= SDL_EVENT_GAMEPAD_AXIS_MOTION &&
        event->type <= SDL_EVENT_GAMEPAD_TOUCHPAD_UP) {
        bongo_cat_window_snapshot_end(app);
        bongo_cat_gamepad_event(app, event);
    }
}

/* elapsed 从上一帧算起并夹在 250ms 以内: 拖窗口/系统休眠后时间差可能是几秒, 不夹住
   模型会瞬移一大步。smoke_freeze_model 让 CI 拿到可复现的画面。 */
static void update_model(BongoCatApp *app, uint64_t now) {
    float elapsed = (float)((now - app->last_frame_ns) / 1000000000.0);
    if (elapsed > 0.25f) elapsed = 0.25f;
    app->last_frame_ns = now;
    if (!app->smoke_freeze_model) bongo_cat_app_step_live2d(app, elapsed);
}

/* present=false 是离屏截帧 (模型封面 / 冒烟回读): 照常画一帧但不交换缓冲。返回 false
   表示这一帧的"目的"没达成 (例如封面回读还没回来), 画面本身可能已经画过了, 调用方
   据此决定是否下一帧重试 —— 所以它不等于渲染失败。 */
static bool render(BongoCatApp *app, bool present) {
    if (present && app->window_snapshot) {
        bongo_cat_window_snapshot_present(app);
        return app->window_snapshot != NULL;
    }
    /* Vulkan 后端有自己的一整条帧路径 (交换链获取/绘制/呈现), 这里直接分叉,
       不碰任何 OpenGL 状态。 */
    if (bongo_cat_renderer_active() == BONGO_CAT_RENDERER_VULKAN) {
        if (!bongo_cat_vk_render_main(app, present)) return false;
        if (present) return true;
        /*
         * 离屏截帧 (模型封面): 与 GL 路径在同一处收尾 —— vk_render_main 刚把这一帧
         * 渲染并呈现, 同时把整帧入队回读; 这里尝试取像素写 PNG。第一次通常还没
         * 回来 (回读有一帧延迟), 于是返回 false 交给上层 defer, 下一帧才写成。
         */
        int width = 0, height = 0;
        SDL_GetWindowSizeInPixels(app->window, &width, &height);
        return bongo_cat_model_cover_capture(app, width, height);
    }
/* GL 上下文可能被设置窗口或其他路径抢走/暂时失效: 失败时标脏并在 1s 后重试, 不当成
   致命错误。Vulkan 路径没有这个问题 (它自己管上下文)。 */
    uint64_t now = SDL_GetTicksNS();
    if (app->render_retry_ns > now) return false;
    if (!SDL_GL_MakeCurrent(app->window, app->gl_context)) {
        app->dirty = true;
        app->render_retry_ns = now + 1000000000ull;
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
            "Main GL context could not be activated: %s", SDL_GetError());
        return false;
    }
    app->render_retry_ns = 0;
    bongo_cat_window_apply_pending_resize(app);
    int width, height;
    SDL_GetWindowSizeInPixels(app->window, &width, &height);
    glViewport(0, 0, width, height);
    glEnable(GL_MULTISAMPLE);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    bongo_cat_window_clear_background(app);
    int content_x = 0, content_y = 0, content_width = width,
        content_height = height;
    bool content_viewport = bongo_cat_live2d_viewport(app->live2d,
        &content_x, &content_y, &content_width, &content_height) &&
        content_width > 0 && content_height > 0;
    if (content_viewport)
        glViewport(content_x, content_y, content_width, content_height);
    bongo_cat_overlay_draw_background(app->overlay,
        app->settings.model.mirror);
    bool cover_requested = !present && bongo_cat_model_cover_pending(app);
    bool cover_ready = !cover_requested ||
        bongo_cat_live2d_prepare_cover_capture(app->live2d);
    glViewport(0, 0, width, height);
    bongo_cat_live2d_set_mirror(app->live2d, app->settings.model.mirror);
    bongo_cat_live2d_draw(app->live2d);
    if (content_viewport)
        glViewport(content_x, content_y, content_width, content_height);
    bongo_cat_overlay_draw_pointer_before_keys(app->overlay);
    if (app->settings.model.mouse_centered && app->pointer_known &&
        !app->model_pointer_anchor_ready)
        bongo_cat_app_apply_mouse_position(app, app->pointer_x,
            app->pointer_y, 0.0f);
    bongo_cat_overlay_draw_keys(app->overlay, app->settings.model.mirror);
    bongo_cat_overlay_draw_effect(app->overlay, app->settings.model.mirror);
    bongo_cat_overlay_draw_pointer_after_keys(app->overlay);
    glViewport(0, 0, width, height);
    bool cover_handled = true;
    if (cover_requested) {
        if (!cover_ready) {
            bongo_cat_model_cover_defer(app, "model-state-preparation");
            cover_handled = false;
        } else if (!bongo_cat_model_cover_capture(app, width, height)) {
            bongo_cat_model_cover_defer(app, "framebuffer-capture");
            cover_handled = false;
        }
    }
    if (cover_requested) app->dirty = true;
    if (!present) {
        app->dirty = true;
        return cover_handled;
    }
    bool reveal_startup = app->startup_visibility_pending &&
        app->session.window.visible;
    bongo_cat_window_mask_corners(app, width, height);
    bongo_cat_frame_audit(app, width, height);
    bongo_cat_window_capture_pointer_hit(app);
/* reveal_startup 与 app.c 的 startup_visibility_pending 配对: 只有"第一帧真的端出去
   了"才清位; 呈现失败要退回隐藏态, 下一轮重来。 */
    /* Keep the native window hidden while diagnostics/readback finish. The
       reveal is intentionally adjacent to the swap so an uninitialised front
       buffer cannot be displayed as a black startup frame. */
    bool pre_presented = reveal_startup &&
        bongo_cat_platform_present(&app->platform, width, height);
    if (reveal_startup)
        bongo_cat_platform_set_visible(&app->platform, true);
    bool presented = pre_presented ||
        bongo_cat_platform_present(&app->platform, width, height);
    if (!presented) {
        if (reveal_startup) bongo_cat_platform_set_visible(&app->platform, false);
        app->dirty = true;
        app->render_retry_ns = now + 1000000000ull;
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
            "Main frame presentation failed: %s", SDL_GetError());
        return false;
    }
    if (reveal_startup) app->startup_visibility_pending = false;
    bongo_cat_frame_presented_audit(app);
    app->input_diagnostics.presented_frames++;
    bongo_cat_startup_ready(app);
    bongo_cat_memory_policy_frame_presented();
    app->dirty = false;
    bongo_cat_window_sync_click_through(app);
    bongo_cat_window_schedule_hit_check(app);
    return true;
}

void bongo_cat_app_render_now(BongoCatApp *app) {
    if (app && app->window && app->session.window.visible &&
        !app->window_minimized)
        render(app, true);
}

bool bongo_cat_app_capture_pending_model_cover(BongoCatApp *app) {
    if (!app || !app->window || !bongo_cat_model_cover_pending(app)) return false;
    if (app->window_snapshot) return false;
    uint64_t now = SDL_GetTicksNS();
    if (!bongo_cat_model_cover_capture_due(app, now)) return false;
    if (app->window_minimized) {
        SDL_Log("Pending model cover deferred while window is minimized: %s",
            bongo_cat_model_cover_pending_path(app));
        bongo_cat_model_cover_defer(app, "window-minimized");
        return false;
    }
    SDL_Log("[runtime] Capturing pending model cover: model=%s loading=%s "
        "visible=%d dirty=%d motions=%zu expression=%d path=%s",
        app->loaded_model,
        app->loading_model[0] ? app->loading_model : "none",
        app->session.window.visible, app->dirty,
        bongo_cat_app_selected_motion_count(app),
        bongo_cat_live2d_expression(app->live2d),
        bongo_cat_model_cover_pending_path(app));
    /* 截帧需要切到主窗口的 GL 上下文; 调用前可能 current 的是别的上下文 (设置窗口),
       必须在返回前恢复, 否则调用方后续的 GL 调用会打到错误的上下文。 */
    SDL_Window *previous_window = SDL_GL_GetCurrentWindow();
    SDL_GLContext previous_context = SDL_GL_GetCurrentContext();
    bool restore_context = previous_window && previous_context &&
        (previous_window != app->window || previous_context != app->gl_context);
    bool captured = render(app, false);
    if (restore_context &&
        !SDL_GL_MakeCurrent(previous_window, previous_context))
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
            "Cannot restore the OpenGL context after model cover capture: %s",
            SDL_GetError());
    return captured;
}

/* 第二个实例被拒绝时会把"唤醒"信号留下来, 主实例在循环里取走: 显示窗口并置前。
   首帧还没出来时只显示、不动 Z 序, 免得把空窗口提到最前面。 */
static void take_instance_wake(BongoCatApp *app) {
    if (!bongo_cat_platform_single_instance_take_wake()) return;
    bongo_cat_window_set_visible(app, true);
    if (!app->startup_visibility_pending)
        bongo_cat_platform_raise_window(app->window);
    SDL_Log("Existing instance requested window reveal");
}

/* 更新器装完包要求本进程退出: 只置 running=false, 真正的清理交给统一的 shutdown。 */
static bool take_update_shutdown(BongoCatApp *app) {
    if (!bongo_cat_platform_single_instance_take_update_shutdown())
        return false;
    app->running = false;
    SDL_LogInfo(BONGO_CAT_LOG_UPDATE,
        "Installed update requested application shutdown");
    return true;
}

void bongo_cat_app_loop(BongoCatApp *app) {
    while (app->running) {
        bongo_cat_app_apply_frame_pacing(app);
        int wait_ms = bongo_cat_window_wait_timeout(app, SDL_GetTicksNS());
        if (app->secondary_pet && wait_ms > 100) wait_ms = 100;
        bongo_cat_preferences_input_begin(app->preferences);
        SDL_Event event;
        if (bongo_cat_wait_event(&event, wait_ms)) {
            handle_event(app, &event);
            unsigned queued = 0;
            while (queued++ < 256 && SDL_PollEvent(&event))
                handle_event(app, &event);
        }
        bongo_cat_preferences_input_end(app->preferences);
        uint64_t now = SDL_GetTicksNS();
        bongo_cat_preferences_model_watch(app->preferences, now);
        bongo_cat_model_refresh_update(app);
        take_instance_wake(app);
        if (take_update_shutdown(app)) continue;
        now = SDL_GetTicksNS();
        bongo_cat_window_update_wheel_animation(app, now);
        bongo_cat_window_snapshot_update(app, now);
        bongo_cat_multi_pet_update(app, now);
        bongo_cat_random_expression_update(app, now);
        bongo_cat_audio_update(app->audio);
        bongo_cat_window_update_display_recovery(app, now);
        bongo_cat_window_update_inertia(app, now);
        bongo_cat_runtime_flow_update(app, now);
        bongo_cat_window_apply_pending_resize(app);
        bongo_cat_app_drain_input(app, true);
        now = SDL_GetTicksNS();
        bongo_cat_app_update_hover(app, now);
        bongo_cat_app_update_hover_fade(app, now);
        if (bongo_cat_model_frame_due(app, now)) update_model(app, now);
        else if (!app->session.window.visible || app->window_minimized) {
            app->last_frame_ns = now;
            bongo_cat_memory_policy_idle();
        }
        bongo_cat_app_capture_pending_model_cover(app);
        if (app->session.window.visible && !app->window_minimized && app->dirty)
            render(app, true);
        bongo_cat_preferences_render(app->preferences);
        /* 渲染两次是刻意的: 设置窗口的渲染与"选模型"处理会改动应用状态并置脏, 第二遍
           只处理这种刚被改脏的情况 (不脏就不会重画)。 */
        bongo_cat_preferences_process_model_selection(app->preferences);
        if (app->session.window.visible && !app->window_minimized && app->dirty)
            render(app, true);
        bongo_cat_tray_sync(app->tray);
        bongo_cat_window_raise_when_due(app, now);
        bongo_cat_config_store_update(app, now);
        /* 自动重启排在 config_store 之后: 新设置必须先落盘, 新进程才读得到。 */
        bongo_cat_app_update_restart(app);
        if (app->smoke_deadline_ns && now >= app->smoke_deadline_ns)
            app->running = false;
    }
}