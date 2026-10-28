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
#include "preferences_live2d_setup.h"
#include "preferences_model_cover.h"
#include "preferences_notice.h"
#include "preferences_state.h"
#include "ui_animation.h"
#include "bongo_cat/i18n.h"
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
    value->vk_consecutive_failures = 0;
    /* 自检面板是窗口内的浮层, 窗口没了它也不能留着 (否则下次开窗会先冒出旧结果) */
    bongo_cat_preferences_selfcheck_reset(value);
    /*
     * 只有 GL 窗口才碰 GL: 主窗口是 Vulkan 时 app->gl_context 为 NULL, 这里的
     * SDL_GL_SetSwapInterval 会进 nvoglv64.dll 崩掉 (与 preferences_scale.c 里
     * 那处同一类问题)。
     *
     * app->gl_context 这条判据是后加的: 上面的 !prefer_vulkan 只说明"设置窗口这次
     * 用的是 GL 通路", 而主窗口完全可能仍是 Vulkan (Vulkan 设置窗口失败后回退到
     * GL 就是这个组合)。那时 app->gl_context 是 NULL, 调 SDL_GL_SetSwapInterval
     * 等于让驱动去操作一个不存在的上下文。
     */
    if (!value->prefer_vulkan && value->app->gl_context) {
        SDL_GL_MakeCurrent(value->app->window, value->app->gl_context);
        SDL_GL_SetSwapInterval(1);
    }
}

/*
 * 防御性回退: 设置窗口的 Vulkan 通路初始化失败或连续提交不出去时, 把这个窗口整个
 * 换成 OpenGL 再打开。
 *
 * 为什么值得这么做: 用户打开设置窗口是为了改设置, 而"渲染后端"这一项就在里面 ——
 * 一个画不出来的设置窗口等于把用户锁死在坏后端上 (这正是用户反馈里的现场)。
 *
 * 三个不变量:
 *   1. 只能在帧外调用 (render 的开头)。它会销毁窗口/UI 后端/全部纹理, 绘制中途调用
 *      会留下一堆悬空 id;
 *   2. vk_disabled 必须粘住, 否则下一次开窗又会按渲染后端切回 Vulkan, 用户看到的是
 *      "界面时好时坏";
 *   3. 回退只能整窗重建 —— SDL_WINDOW_VULKAN 的窗口不允许再建 GL 上下文。
 */
void bongo_cat_preferences_vulkan_fallback(BongoCatPreferences *value) {
    if (!value) return;
    value->vk_fallback_requested = false;
    const unsigned consecutive = value->vk_consecutive_failures;
    const uint64_t total = value->vk_failures;
    if (!value->prefer_vulkan) return;
    SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
        "[render] 设置窗口的 Vulkan 通路不可用 (连续失败 %u 次, 累计 %llu 次): "
        "改用 OpenGL 重建这个窗口 (主窗口的后端不受影响)",
        consecutive, (unsigned long long)total);
    value->vk_disabled = true;
    /* 顺序要紧: 先改标志再 release_window, 让它内部的 GL 复原分支不去碰 Vulkan 主窗口 */
    value->prefer_vulkan = false;
    bool was_shown = value->visible;
    value->visible = false;
    release_window(value);
    if (value->app)
        bongo_cat_preferences_notice_show(value->app,
            bongo_cat_i18n_get(value->app->i18n,
                "composables.useAppMenu.labels.rendererRestart",
                "Restart BongoCat to apply the new render backend."), true);
    if (!bongo_cat_preferences_open_window(value)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Preferences OpenGL fallback window could not be created: %s",
            SDL_GetError());
        return;
    }
    /* 用户本来就是开着它的: 回退后要立刻重新显示, 不能变成"设置窗口自己消失了"。 */
    if (was_shown) {
        value->visible = true;
        value->shown_ns = SDL_GetTicksNS();
        SDL_StartTextInput(value->window);
        bongo_cat_preferences_live_resize_install(value);
        value->render_dirty = true;
        bongo_cat_preferences_render(value);
        bongo_cat_platform_raise_window(value->window);
    }
    SDL_Log("[render] 设置窗口已回退到 OpenGL 并重建 (连续失败 %u 次, 累计 %llu 次)",
        consecutive, (unsigned long long)total);
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
    /* 缺 Core / 缺渲染组件时每次运行只提示一次: 放在"窗口确实开起来了"之后, 免得
       窗口创建失败时提示也跟着丢掉, 用户完全不知道 Live2D 为什么没渲染。 */
    bongo_cat_preferences_live2d_setup_notice(value->app);
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
