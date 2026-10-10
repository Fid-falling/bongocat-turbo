#include "preferences_state.h"
#include "preferences_render_internal.h"
#include "preferences_controls.h"
#include "ui_animation.h"
#include "ui_paint.h"
#include "bongo_cat/memory_policy.h"

#include <SDL3/SDL_opengl.h>

/*
 * 冒烟/排查用的分阶段日志。默认关闭: 设置窗口每帧都走一遍这里, 开着的时候一次
 * 会话就能把日志写到几 MB (实测 2.3 MB), 还白做每帧的字符串格式化。
 * 需要时用 BONGO_CAT_TEST_PREFS_STAGES=1 打开。
 */
static bool stages_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) enabled = SDL_getenv("BONGO_CAT_TEST_PREFS_STAGES") != NULL;
    return enabled != 0;
}

static void stage_log(const char *stage) {
    if (stages_enabled())
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "[prefs] stage=%s", stage);
}

/*
 * 设置界面的渲染节流: 60 Hz。
 *
 * 界面是事件驱动渲染 (滚动/拖动/动画期间 render_dirty 会一直为真), 而它是挂在
 * 宠物窗口的主循环里跑的 —— 宠物窗口设成"无限制"时主循环能到 800 Hz, 设置界面
 * 就会以同样的频率去 present。开着 RTSS (微星小飞机 OSD) / OBS 这类叠加层钩子时,
 * 每次 present 都要过一遍钩子, 于是被放大成严重掉帧。节流后 present 次数与刷新率
 * 对齐, 滚动依旧是逐帧的, 观感不变。
 */
#define BONGO_CAT_PREFERENCES_FRAME_NS 16666667ull

void bongo_cat_preferences_render(BongoCatPreferences *value) {
    bongo_cat_preferences_release_idle_window(value);
    if (!value || !value->window || !value->visible) return;
    bongo_cat_preferences_drag_tick(value);
    /* [性能]区: 关掉发光/阴影类特效 (浅色主题下这些全屏模糊是每帧最贵的一项) */
    bongo_cat_ui_set_effects_enabled(!value->app->settings.window.disable_effects);
    uint64_t now = SDL_GetTicksNS();
    bool raster_due = value->pending_raster_scale > 0.0f &&
        value->raster_retry_ns <= now;
    if (value->render_retry_ns > now ||
        (!value->render_dirty && value->last_render_ns && !raster_due)) return;
    /* 这里不清 render_dirty: 这一轮跳过, 下一轮主循环接着画 */
    if (value->last_render_ns && !raster_due &&
        now - value->last_render_ns < BONGO_CAT_PREFERENCES_FRAME_NS) return;
    value->render_dirty = false;
    value->ui.context.delta_time_seconds = value->last_render_ns ? NK_CLAMP(
        .001f, (float)(now - value->last_render_ns) / 1000000000.0f,
        1.0f / 60.0f) : 1.0f / 60.0f;
    value->last_render_ns = now;
    bongo_cat_preferences_input_end(value);
    const bool vulkan = bongo_cat_ui_uses_vulkan(&value->ui);
    if (!vulkan && !SDL_GL_MakeCurrent(value->window, value->gl_context)) {
        value->render_dirty = true;
        value->render_retry_ns = now + 1000000000ull;
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "Preferences GL context could not be activated: %s", SDL_GetError());
        return;
    }
    value->render_retry_ns = 0;
    bool importing = bongo_cat_preferences_import_status(
        value->import_dialog, NULL, NULL, NULL);
    value->import_render_active = importing;
    bool refreshing_models = bongo_cat_app_model_refresh_busy(value->app);
    if (!importing && !refreshing_models) {
        bongo_cat_preferences_refresh_raster(value);
        bongo_cat_preferences_reload_language(value);
    }
    if (value->font_reload_pending && !importing && !refreshing_models) {
        if (value->font_reload_defer_once) {
            value->font_reload_defer_once = false;
            value->render_dirty = true;
        } else if (!bongo_cat_preferences_reload_fonts(value)) {
            value->font_reload_pending = false;
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "Deferred preferences font atlas rebuild failed");
        }
    }
    bongo_cat_preferences_apply_theme(value);
    float width = 0.0f, height = 0.0f;
    bongo_cat_ui_logical_size(&value->ui, &width, &height);
    bongo_cat_ui_paint_begin_frame(&value->ui);
    stage_log("paint-begin");
    bongo_cat_ui_cursor_begin(&value->ui);
    bool dark = bongo_cat_preferences_resolved_theme(value) != 0;
    value->ui.frame_building = true;
    bool close_requested = bongo_cat_preferences_draw_frame(
        value, width, height, dark);
    value->ui.frame_building = false;
    stage_log("drawn");
    bongo_cat_preferences_shortcut_smoke(value);
    BongoCatUIPalette palette = bongo_cat_ui_palette(dark);
    if (vulkan) {
        /* Vulkan: 清屏由渲染通道的 clear 值完成, 与 GL 的 glClearColor 对应 */
        int pixel_width = 0, pixel_height = 0;
        SDL_GetWindowSizeInPixels(value->window, &pixel_width, &pixel_height);
        bongo_cat_vk_set_clear_color(value->vk,
            value->transparent_window ? 0.0f : palette.background.r / 255.0f,
            value->transparent_window ? 0.0f : palette.background.g / 255.0f,
            value->transparent_window ? 0.0f : palette.background.b / 255.0f,
            value->transparent_window ? 0.0f : 1.0f);
        if (!bongo_cat_vk_begin_frame(value->vk, pixel_width, pixel_height)) {
            value->render_dirty = true;
            value->render_retry_ns = now + 1000000000ull;
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "Preferences Vulkan frame failed");
            return;
        }
    } else {
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(
            value->transparent_window ? 0.0f : palette.background.r / 255.0f,
            value->transparent_window ? 0.0f : palette.background.g / 255.0f,
            value->transparent_window ? 0.0f : palette.background.b / 255.0f,
            value->transparent_window ? 0.0f : 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    bongo_cat_ui_render(&value->ui);
    stage_log("ui-rendered");
    bongo_cat_preferences_smoke_frame(value);
    stage_log("smoke-done");
    bool presented = true;
    const uint64_t present_start_ns = SDL_GetTicksNS();
    if (vulkan) {
    stage_log("present-begin");
        presented = bongo_cat_vk_end_frame(value->vk);
            if (!presented)
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "Preferences Vulkan present failed");
    } else if (!SDL_GL_SwapWindow(value->window)) {
        presented = false;
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "Preferences frame presentation failed: %s", SDL_GetError());
    }
    if (!presented) {
        value->render_dirty = true;
        value->render_retry_ns = now + 1000000000ull;
    } else bongo_cat_memory_policy_ui_frame_presented();
    stage_log("present-done");
    /*
     * 每帧耗时拆成两段: 整帧 (从 render() 开头算) 和 present 本身。开着 RTSS /
     * 微星小飞机 OSD 时 present 会被钩子拖长, 这两个数字能直接说明瓶颈在谁身上
     * (用 BONGO_CAT_TEST_PREFS_STAGES=1 打开)。
     */
    if (stages_enabled()) {
        const uint64_t present_end_ns = SDL_GetTicksNS();
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "[prefs] frame ms=%.2f present_ms=%.2f vulkan=%d",
            (double)(present_end_ns - now) / 1000000.0,
            (double)(present_end_ns - present_start_ns) / 1000000.0,
            vulkan ? 1 : 0);
    }
    bongo_cat_preferences_record_frame(value);
    stage_log("record-done");
    if (value->shortcut_recording || value->model_load_visual_active ||
        importing || bongo_cat_pref_controls_animating(&value->ui.context) ||
        bongo_cat_ui_animations_active(&value->ui.context))
        value->render_dirty = true;
    else if (!value->chrome_dragging && !value->live_resize_active)
        bongo_cat_ui_trim_idle(&value->ui);
    if (!vulkan) SDL_GL_MakeCurrent(value->app->window, value->app->gl_context);
    bongo_cat_ui_cursor_apply(&value->ui);
    stage_log("cursor-done");
    if (close_requested) {
        bongo_cat_preferences_close(value);
        return;
    }
    if (value->import_requested && !bongo_cat_preferences_import_is_open(
        value->import_dialog)) {
        value->import_requested = false;
        if (!bongo_cat_preferences_import_open(value->import_dialog,
                value->window))
            value->render_dirty = true;
    }
}
