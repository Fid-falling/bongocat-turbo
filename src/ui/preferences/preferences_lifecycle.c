/*
 * 设置窗口的生死: 建 -> 显示 -> 隐藏 -> 空闲回收 -> 彻底销毁。
 *
 * 关键设计是"窗口可以被回收" (release_idle_window -> release_window): 关窗后并不立刻
 * 销毁窗口, 而是等导入/模型刷新都结束, 再把窗口、UI 后端、纹理缓存全部放掉并把工作集
 * 还给系统 —— 任务管理器上能直接看到内存下降。BongoCatPreferences 结构体本身留着,
 * 所以下次 show 是"重建"而不是"新建"。
 *
 * 这里有两条踩过坑的规矩:
 * 1. 释放前必须先确认 GL 上下文可用 (context_ready)。用不了时不能直接不管 ——
 * 那样纹理 id 会留在已经销毁的后端里; 正确做法是走 abandon 系列 (连表一起丢)
 * 而不是 clear 系列 (逐个 release);
 * 2. 只有 GL 窗口才能碰 SDL_GL_*; 主窗口是 Vulkan 时 app->gl_context 为 NULL,
 * 调 SDL_GL_SetSwapInterval 会进 nvoglv64.dll 崩掉 (见函数末尾)。
 */
#include "preferences_controls.h"
#include "preferences_gl.h"
#include "preferences_model_cover.h"
#include "preferences_state.h"
#include "ui_animation.h"
#include "bongo_cat/memory.h"
#include "bongo_cat/platform.h"
#include "bongo_cat/preferences.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <stdlib.h>

/*
 * 真正把窗口与它名下的一切放掉。顺序是精心排的:
 * 1. 先卸掉 live_resize 的窗口子类化 —— HWND 一旦销毁就再也拿不到原窗口过程,
 * 而子类化过程里还存着指向本结构体的指针;
 * 2. 按上下文是否可用, 在 clear / abandon 之间二选一 (纹理、模型封面、UI 素材);
 * 3. 销毁 GL/Vulkan 上下文, 最后才销毁窗口。
 * 收尾把所有"窗口存在期间才有意义"的标志复位, 但保留用户选择 (比如当前页) —— 那个
 * 是跨窗口会话, 下次打开应该还在原来那一页。
 */
static void release_window(BongoCatPreferences *value) {
    if (!value || !value->window) return;
    bongo_cat_preferences_live_resize_uninstall(value);
    bool context_ready = !value->gl_context ||
        SDL_GL_MakeCurrent(value->window, value->gl_context);
    if (!context_ready) SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
        "Preferences GL cleanup skipped because its context could not be "
        "activated: %s", SDL_GetError());
    if (value->ui_initialized && value->input_active)
        bongo_cat_preferences_input_end(value);
    SDL_StopTextInput(value->window);
    if (context_ready) bongo_cat_preferences_model_cache_clear(value->app);
    else bongo_cat_preferences_model_cache_abandon(value->app);
    if (value->ui_initialized) {
        bongo_cat_pref_controls_reset(&value->ui.context);
        bongo_cat_ui_animations_reset(&value->ui.context);
    }
    if (context_ready) bongo_cat_preferences_assets_clear(value);
    else bongo_cat_preferences_assets_abandon(value);
    if (value->ui_initialized) {
        bongo_cat_ui_cursor_destroy(&value->ui);
        if (context_ready) bongo_cat_ui_destroy(&value->ui);
        else bongo_cat_ui_abandon(&value->ui);
    }
    if (value->chrome_dragging) SDL_CaptureMouse(false);
    bongo_cat_preferences_gl_destroy(value);
    SDL_DestroyWindow(value->window);
    /*
     * 窗口、UI 后端与纹理缓存 (paint cache) 都放掉了: 顺手把空闲堆和工作集还给
     * 系统, 否则这些页还留在工作集里, 任务管理器上看不出下降。
     */
    bongo_cat_platform_trim_memory();
    value->window = NULL;
    value->visible = false;
    value->transparent_window = false;
    value->ui_initialized = false;
    value->font_reload_pending = false;
    value->font_reload_defer_once = false;
    value->model_load_visual_active = false;
    value->model_load_visual_completion_ns = 0;
    value->smoke_behavior_open_pending = false;
    value->native_drag = false;
    value->chrome_dragging = false;
    value->pending_raster_scale = 0.0f;
    value->raster_retry_ns = 0;
    value->render_retry_ns = 0;
    value->shown_ns = 0;
    /*
     * 只有 GL 窗口才碰 GL: 主窗口是 Vulkan 时 app->gl_context 为 NULL, 这里的
     * SDL_GL_SetSwapInterval 会进 nvoglv64.dll 崩掉 (与 preferences_scale.c 里
     * 那处同一类问题)。
     */
    if (!value->prefer_vulkan) {
        SDL_GL_MakeCurrent(value->app->window, value->app->gl_context);
        SDL_GL_SetSwapInterval(1);
    }
}

/*
 * 显示设置窗口 (托盘菜单/快捷键都会走这里)。
 * 已经可见时只做"抬到最前 + 请求刷新附近的模型"就返回 —— 重复建窗会丢掉当前状态。
 * 建窗失败时 release_window 兜底, 保证不会留下半个窗口的状态。
 * 结尾立刻同步渲染一帧: 用户点完菜单要马上看到窗口内容, 不能等下一次事件。
 */
void bongo_cat_preferences_show(BongoCatPreferences *value) {
    if (!value) return;
#ifdef __APPLE__
    bongo_cat_preferences_input_monitoring_refresh(value);
#endif
    if (value->visible) {
        bongo_cat_platform_raise_window(value->window);
        bongo_cat_app_request_nearby_model_refresh(value->app);
        return;
    }
    uint64_t requested_ns = SDL_GetTicksNS();
    bool opening = !value->window;
    if (opening && !bongo_cat_preferences_open_window(value)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Preferences failed: %s", SDL_GetError());
        release_window(value);
        return;
    }
    if (value->ui_initialized) bongo_cat_ui_input_reset(&value->ui);
    value->shown_ns = requested_ns;
    value->visible = true;
    if (!opening) {
        SDL_StartTextInput(value->window);
        bongo_cat_preferences_live_resize_install(value);
    }
    value->render_dirty = true;
    bongo_cat_preferences_render(value);
    bongo_cat_platform_raise_window(value->window);
    bongo_cat_app_request_nearby_model_refresh(value->app);
}

/*
 * 关闭 (= 隐藏) 设置窗口。
 * 这里做的是"把正在进行的一切收干净再藏起来": 关掉行为对话框、保存未提交的改名、
 * 取消快捷键录制、退出文本输入、松开鼠标捕获。少任何一项, 下次打开都会带着上一次的
 * 残留状态 (典型症状: 刚打开就有一个输入框在吃键盘)。
 * 藏起来之后交给 release_idle_window 决定能不能真正回收窗口; 最后 flush 配置。
 */
void bongo_cat_preferences_close(BongoCatPreferences *value) {
    if (!value || !value->window || !value->visible) return;
    bongo_cat_preferences_live_resize_uninstall(value);
    if (bongo_cat_preferences_behavior_dialog_active(value))
        bongo_cat_preferences_behavior_dialog_close(value);
    bongo_cat_preferences_model_rename_finish(value, true);
    bongo_cat_preferences_shortcut_cancel(value);
    value->behavior_dialog = false;
    value->behavior_dialog_input_armed = false;
    value->behavior_dialog_opened_ns = 0;
    value->behavior_dialog_closing_ns = 0;
    value->import_requested = false;
    value->import_drop_active = false;
    if (value->input_active) bongo_cat_preferences_input_end(value);
    if (value->ui_initialized) bongo_cat_ui_input_reset(&value->ui);
    SDL_StopTextInput(value->window);
    if (value->chrome_dragging) SDL_CaptureMouse(false);
    value->chrome_dragging = false;
    value->visible = false;
    SDL_HideWindow(value->window);
    bongo_cat_preferences_release_idle_window(value);
    bongo_cat_config_store_flush(value->app);
}

/*
 * 窗口已经藏起来时, 判断能不能把窗口连同 UI 后端一起收掉。
 * 三种情况必须等 (见下面的英文注释): 文件夹对话框还开着 (它需要 owner 窗口)、
 * 后台正在刷新模型目录、正在导入/等待处理模型选择 —— 这些都会在结束后回调到这里,
 * 那时才真正回收。所以本函数在 render 每一帧的开头都会被调一次, 是"延迟回收"的落点。
 */
void bongo_cat_preferences_release_idle_window(BongoCatPreferences *value) {
    if (!value || !value->window || value->visible) return;
    /* A folder dialog still needs its owner; import completion can queue a
       catalog refresh. Keep the window until both have finished. */
    if (bongo_cat_preferences_import_is_open(value->import_dialog) ||
        bongo_cat_app_model_refresh_busy(value->app) || value->model_loading ||
        value->model_selection_pending) return;
    release_window(value);
}

/*
 * 进程退出时的最终销毁: 先销毁导入器 (它会等工作线程收尾), 再关窗, 最后释放结构体。
 * 顺序反了会让工作线程往已经释放的对话框里写状态。
 */
void bongo_cat_preferences_destroy(BongoCatPreferences *value) {
    if (!value) return;
    bongo_cat_preferences_import_destroy(value->import_dialog);
    value->import_dialog = NULL;
    bongo_cat_preferences_close(value);
    release_window(value);
    free(value);
}
