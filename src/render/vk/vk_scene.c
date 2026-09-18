#include "vk_internal.h"
#include "runtime.h"
#include "bongo_cat/log.h"
#include "bongo_cat/overlay.h"
#include "bongo_cat/render.h"
#include "bongo_cat/vk_backend.h"

#include <SDL3/SDL.h>

/*
 * Vulkan 后端的主窗口帧。
 *
 * 绘制顺序与 OpenGL 路径 (app_loop.c 的 render()) 严格一致, 这样两个后端看到
 * 的是同一幅画面:
 *   清屏 -> 叠加层背景 -> 鼠标指针(按键之下) -> 叠加层按键 -> 特效 -> 指针(之上)
 *
 * 与后端无关的状态推进 (输入、动画、命中测试调度) 仍由 app_loop 统一驱动。
 * 当前进度: 设备/交换链/呈现/叠加层与指针绘制已落地; 圆角遮罩与逐像素命中
 * 测试回读属于后续阶段。
 */

bool bongo_cat_vk_render_main(BongoCatApp *app, bool present) {
    if (!app || !app->vk || !app->window) return false;
    if (!present) {
        /* 离屏截帧 (模型封面) 需要把这一帧渲染到可回读的独立图像里, 属于
           后续阶段的 Vulkan 回读路径; 现在直接让调用方延后处理。 */
        return false;
    }
    uint64_t now = SDL_GetTicksNS();
    if (app->render_retry_ns > now) return false;
    int width = 0, height = 0;
    SDL_GetWindowSizeInPixels(app->window, &width, &height);
    if (width <= 0 || height <= 0) return false;

    float rgba[4];
    bongo_cat_window_background_rgba(app, rgba);
    bongo_cat_vk_set_clear_color(app->vk, rgba[0], rgba[1], rgba[2], rgba[3]);
    if (!bongo_cat_vk_begin_frame(app->vk, width, height)) {
        app->dirty = true;
        app->render_retry_ns = now + 1000000000ull;
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "[vulkan] 帧开始失败");
        return false;
    }

    bongo_cat_overlay_draw_background(app->overlay, app->settings.model.mirror);
    bongo_cat_overlay_draw_pointer_before_keys(app->overlay);
    bongo_cat_overlay_draw_keys(app->overlay, app->settings.model.mirror);
    bongo_cat_overlay_draw_effect(app->overlay, app->settings.model.mirror);
    bongo_cat_overlay_draw_pointer_after_keys(app->overlay);

    app->render_retry_ns = 0;
    bool presented = bongo_cat_vk_end_frame(app->vk);
    if (!presented) {
        app->dirty = true;
        app->render_retry_ns = now + 1000000000ull;
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "[vulkan] 帧提交/呈现失败");
        return false;
    }
    /* 首帧已经送到合成器之后才显示窗口, 与 OpenGL 路径一致: 不让用户看到一张
       还没画过的空白表面。 */
    bool reveal_startup = app->startup_visibility_pending &&
        app->session.window.visible;
    if (reveal_startup) {
        bongo_cat_platform_set_visible(&app->platform, true);
        app->startup_visibility_pending = false;
    }
    app->input_diagnostics.presented_frames++;
    app->dirty = false;
    bongo_cat_startup_ready(app);
    bongo_cat_memory_policy_frame_presented();
    bongo_cat_window_sync_click_through(app);
    bongo_cat_window_schedule_hit_check(app);
    return true;
}
