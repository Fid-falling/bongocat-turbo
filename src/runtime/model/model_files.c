/* 模型选择与加载的事务实现：从 id 到屏幕上真的换了模型的全部副作用都在
 * bongo_cat_app_select_model_with_error() 里完成（行为目录、渲染参数、叠加层、几何、音频、
 * 封面、内存策略）。它由主循环或设置窗口在渲染线程同步调用，期间用 model_load_progress
 * 回调插播小帧维持窗口响应；加载失败不会重建旧模型，只保证状态不悬空。改这条路径最常见的
 * 错误是漏掉某个子系统的重置或上下文还原。 */
#include "runtime.h"
#include "model_import.h"
#include "model_behavior_cache.h"
#include "model_cover.h"
#include "model_geometry.h"
#include "bongo_cat/audio.h"
#include "bongo_cat/overlay.h"
#include "bongo_cat/preferences.h"
#include "bongo_cat/render.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* 把目录条目变成当前选择：多宠开启时新主模型要从附加列表里摘掉（否则同一模型被算两次），
 * 关闭时清空附加列表；模式决定是否启用手柄输入。不做渲染或磁盘副作用。 */
static void select_model_state(BongoCatApp *app, const BongoCatModelEntry *entry) {
    if (app->settings.model.multiple_pets)
        bongo_cat_session_remove_model(&app->session, entry->id);
    else bongo_cat_session_clear_additional_models(&app->session);
    snprintf(app->session.active_model_id, sizeof(app->session.active_model_id), "%s",
        entry->id);
    app->loaded_mode = entry->mode;
    bongo_cat_gamepads_set_enabled(app, entry->mode == BONGO_CAT_MODE_GAMEPAD);
}
/* reveal 为真时把隐藏、最小化或悬停隐藏的窗口拉回可见，否则只把越界窗口夹回显示器；无论
 * 哪条路径都会标脏命中测试与 app->dirty，让主循环下一帧出图。 */
static void request_model_frame(BongoCatApp *app, bool reveal) {
    if (!app) return;
    if (app->window) {
        SDL_WindowFlags flags = SDL_GetWindowFlags(app->window);
        bool hidden = (flags & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)) != 0;
        if (reveal && (!app->session.window.visible || hidden ||
            app->hover_hidden))
            bongo_cat_window_set_visible(app, true);
        else if (app->session.window.visible)
            bongo_cat_window_clamp_to_display(app);
    }
    bongo_cat_window_mark_hit_dirty(app);
    app->dirty = true;
}

static void model_runtime_stage(BongoCatApp *app, const char *stage,
    const BongoCatModelEntry *entry) {
    char state[BONGO_CAT_ID_CAP + 32];
    const char *id = entry ? entry->id : "unknown";
    snprintf(state, sizeof(state), "model-load:%s:%s", stage, id);
    bongo_cat_runtime_stage(app, state);
    SDL_Log("[runtime] Model load %s: id=%s", stage, id);
}

static void model_running_stage(BongoCatApp *app, const char *model_id) {
    char state[BONGO_CAT_ID_CAP + 16];
    snprintf(state, sizeof(state), "running:model:%s",
        model_id && model_id[0] ? model_id : "none");
    bongo_cat_runtime_stage(app, state);
}

/* 只负责阶段跳变那一次上报：progress 落进 1/2/3 三档，与上一档相同时直接返回。这里读的是
 * loading_model 而不是 loaded_model —— 加载期间后者仍是旧模型。 */
static void model_progress_runtime_stage(BongoCatApp *app, float progress) {
    static const char *names[] = {"", "model-core", "texture-loading",
        "finalizing"};
    unsigned stage = progress >= .95f ? 3 : progress >= .50f ? 2 : 1;
    if (!app || stage == app->model_load_runtime_stage) return;
    app->model_load_runtime_stage = stage;
    char state[BONGO_CAT_ID_CAP + 40];
    snprintf(state, sizeof(state), "model-load:%s:%s", names[stage],
        app->loading_model[0] ? app->loading_model : "unknown");
    bongo_cat_runtime_stage(app, state);
}

/* Live2D 加载期间被同步回调（仍在主线程，此时主循环被完全阻塞）。做三件事：上报阶段与进
 * 度条，SDL_PumpEvents() 让窗口消息不至于饿死，以及在替换已有模型时按 ~60fps 插播小帧让旧
 * 模型继续动 —— 小帧只在 model_load_last_frame_ns 非零时启用，加载前会置位、加载结束立刻
 * 清零。 */
static void model_load_progress(void *userdata, float progress) {
    BongoCatApp *app = userdata;
    model_progress_runtime_stage(app, progress);
    /* Keep native window procedures responsive while the main loop is
       synchronously loading Cubism and OpenGL resources. */
    SDL_PumpEvents();
    if (app && app->preferences)
        bongo_cat_preferences_model_load_progress(app->preferences, progress);
    /* The main loop is blocked during loading, so advance the visible model here. */
    if (!app || !app->loaded_model[0] || !app->model_load_last_frame_ns ||
        !app->live2d)
        return;
    uint64_t now = SDL_GetTicksNS();
    if (progress < .98f && now - app->model_load_last_frame_ns < 16000000ull)
        return;
    uint64_t previous = app->model_load_last_frame_ns;
    app->model_load_last_frame_ns = now;
    float elapsed = (float)((now - previous) / 1000000000.0);
    bongo_cat_app_drain_input(app, false);
    now = SDL_GetTicksNS();
    bongo_cat_app_update_hover(app, now);
    bongo_cat_app_update_hover_fade(app, now);
    if (!app->smoke_freeze_model && elapsed > 0.0f)
        bongo_cat_app_step_live2d(app, elapsed);
    app->last_frame_ns = now;
    bongo_cat_app_render_now(app);
}

/* 提交选择：id 与模式都没变且未强制刷新时是空操作。model_selection_serial 自增是给 UI 与
 * 封面等观察者识别发生过切换的序号，每次真正提交都要加一。 */
static void commit_model(BongoCatApp *app,
    const BongoCatModelEntry *entry, bool force_refresh, bool reveal) {
    bool changed = strcmp(app->session.active_model_id, entry->id) != 0 ||
        app->loaded_mode != entry->mode;
    if (!changed && !force_refresh) return;
    select_model_state(app, entry);
    app->model_selection_serial++;
    request_model_frame(app, reveal);
}

/* 返回 bool，失败信息写进 *error（传 NULL 时写进内部局部变量，调用方可以不关心）。前置条
 * 件：app 与渲染器已就绪，id 必须存在于 app->models。已经加载同一个 id 时只补一次 commit
 * （模式可能变过），不重新加载。失败返回后 app->models 与 session 不变，但 live2d 可能已经
 * 没有可用模型（loaded_model 被清空）。 */
bool bongo_cat_app_select_model_with_error(BongoCatApp *app,
    const char *id, BongoCatError *error) {
    BongoCatError local = {0};
    BongoCatError *failure = error ? error : &local;
    *failure = (BongoCatError){0};
    if (!app || !app->live2d || !id) {
        bongo_cat_error_set(failure, BONGO_CAT_ERROR_ARGUMENT,
            "Cannot select a model without an active renderer and model id");
        return false;
    }
    const BongoCatModelEntry *entry = bongo_cat_models_find(&app->models, id);
    if (!entry) {
        bongo_cat_error_set(failure, BONGO_CAT_ERROR_ARGUMENT,
            "Model is not installed: %s", id);
        return false;
    }
    /* 快照预览要先补画最后一帧实时画面才能释放，所以必须在切换模型与 GL 上下文之前结束
       它，否则补画用的是已经被替换掉的状态。 */
    bongo_cat_window_snapshot_end(app);
    if (app->loaded_model[0] && strcmp(app->loaded_model, entry->id) == 0) {
        commit_model(app, entry, false, false);
        return true;
    }
    bongo_cat_app_capture_behavior_state(app);
    bool replacing_model = app->loaded_model[0] != '\0';
    if (replacing_model)
        bongo_cat_model_cover_capture_before_switch(app);
    /* 行为目录优先复用 app->behavior_cache（仅限已安装包且内容摘要匹配），否则重新解析。解
       析失败不是致命错误：模型照样显示，只是没有动作与表情；这里只记下 validity 供缓存判
       定使用。 */
    BongoCatError optional = {0};
    BongoCatBehaviorCatalog *behaviors = calloc(1, sizeof(*behaviors));
    if (!behaviors) {
        bongo_cat_error_set(failure, BONGO_CAT_ERROR_MEMORY,
            "Cannot allocate model behavior state");
        return false;
    }
    bool behavior_catalog_valid = bongo_cat_model_behavior_cache_matches(
        app, entry);
    if (behavior_catalog_valid) {
        *behaviors = *app->behavior_cache;
    } else {
        behavior_catalog_valid = bongo_cat_behaviors_load(
            behaviors, entry, &optional) == BONGO_CAT_OK;
        if (!behavior_catalog_valid)
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s", optional.message);
    }
    /* 适配器 profile 是导入时生成的运行时参数（投影、指针边界等）；读不到就用零初始化的默
       认值。它同时写进 app->model_render_options 供后续帧复用。 */
    BongoCatLive2DRenderOptions render_options = {0};
    bool adapted_profile = bongo_cat_import_render_options(
        entry->adapter_directory, &render_options);
    if (adapted_profile) SDL_Log("Imported runtime profile: projection=%.4f "
        "force_mouse=%d left_handed=%d pointer_bounds=%d",
        render_options.projection_scale, render_options.mouse_force_move,
        render_options.pointer_left_handed, render_options.custom_pointer_bounds);
    BongoCatModelContentAnchor content_anchor =
        bongo_cat_model_content_anchor(app);
    int pixel_width = app->session.window.width, pixel_height = app->session.window.height;
    if (app->window) SDL_GetWindowSizeInPixels(app->window, &pixel_width, &pixel_height);
    SDL_Window *previous_window = SDL_GL_GetCurrentWindow();
    SDL_GLContext previous_context = SDL_GL_GetCurrentContext();
    /* Vulkan 后端没有 OpenGL 上下文: 模型加载不再切换 GL 上下文, 也不读取
       glGetError, 其余流程完全一致。 */
    const bool gl_backend =
        bongo_cat_renderer_active() != BONGO_CAT_RENDERER_VULKAN;
    /* 只有 OpenGL 后端需要临时切换上下文；成功与失败两条出口都会还原入场时的窗口与上下
       文，不把调用方的 GL 状态改掉。 */
    bool restore_context = gl_backend && (previous_window != app->window ||
        previous_context != app->gl_context);
    if (restore_context && !SDL_GL_MakeCurrent(app->window, app->gl_context)) {
        bongo_cat_error_set(failure, BONGO_CAT_ERROR_PLATFORM,
            "Cannot activate the main OpenGL context: %s", SDL_GetError());
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", failure->message);
        free(behaviors);
        return false;
    }
    char previous_model[BONGO_CAT_ID_CAP];
    snprintf(previous_model, sizeof(previous_model), "%s",
        app->loaded_model[0] ? app->loaded_model : "none");
    uint64_t load_started_ns = SDL_GetTicksNS();
    GLenum initial_gl_error = gl_backend ? glGetError() : 0;
    SDL_Log("[runtime] Model switch transaction: stage=begin previous=%s next=%s "
        "main_window=%p main_context=%p current_window=%p current_context=%p "
        "gl_error=0x%x", previous_model, entry->id, (void *)app->window,
        (void *)app->gl_context, (void *)SDL_GL_GetCurrentWindow(),
        (void *)SDL_GL_GetCurrentContext(), (unsigned)initial_gl_error);
    /* 先用当前像素尺寸 reshape，让新模型在加载期就按最终窗口构图；加载完成后几何还可能变
       化，那时会再 resize 一次。 */
    bongo_cat_live2d_reshape(app->live2d, pixel_width, pixel_height);
    snprintf(app->loading_model, sizeof(app->loading_model), "%s", entry->id);
    app->model_load_runtime_stage = 0;
    model_runtime_stage(app, "started", entry);
    app->model_load_last_frame_ns = replacing_model ? SDL_GetTicksNS() : 0;
    BongoCatResult load_result = bongo_cat_live2d_load(app->live2d, entry->directory,
        entry->setting_file, entry->preset, &render_options,
        model_load_progress, app, failure);
    app->model_load_last_frame_ns = 0;
    if (replacing_model) app->last_frame_ns = SDL_GetTicksNS();
    /* 失败时旧模型通常已被销毁，所以只在 live2d 确实不可用时清空 loaded_model；若渲染器仍
       能画出旧模型就保留它，让用户看到的是没换成而不是黑屏。behaviors 在这里释放，
       app->behaviors 保持不变。 */
    if (load_result != BONGO_CAT_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Model switch transaction: "
            "stage=failed previous=%s next=%s elapsed_ms=%.2f gl_error=0x%x "
            "error=%s", previous_model, entry->id,
            (double)(SDL_GetTicksNS() - load_started_ns) / 1000000.0,
            gl_backend ? (unsigned)glGetError() : 0u, failure->message);
        if (restore_context && previous_window && previous_context &&
            !SDL_GL_MakeCurrent(previous_window, previous_context))
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "Cannot restore the previous OpenGL context: %s", SDL_GetError());
        free(behaviors);
        if (!bongo_cat_live2d_ready(app->live2d)) app->loaded_model[0] = '\0';
        request_model_frame(app, false);
        app->loading_model[0] = '\0';
        app->model_load_runtime_stage = 0;
        model_running_stage(app, app->loaded_model);
        return false;
    }
    model_runtime_stage(app, "committing", entry);
    BongoCatParameterRange pointer_x, pointer_y;
    bool model_pointer =
        bongo_cat_live2d_parameter(app->live2d, "ParamMouseX", &pointer_x) &&
        bongo_cat_live2d_parameter(app->live2d, "ParamMouseY", &pointer_y) &&
        pointer_x.maximum > pointer_x.minimum &&
        pointer_y.maximum > pointer_y.minimum;
    app->model_render_options = render_options;
    bongo_cat_app_reset_pointer_tracking(app);
    bongo_cat_live2d_set_render_options(app->live2d, &render_options);
    /* 覆盖 app->behaviors 之前，先把上一个模型的目录存进缓存（仅当缓存原本有效且确实有上
       一个模型），供用户切回去时复用；否则显式作废缓存，避免摘要与内容对不上。 */
    if (app->loaded_model[0] && app->behavior_catalog_valid) {
        const BongoCatModelEntry *previous_entry = bongo_cat_models_find(
            &app->models, app->loaded_model);
        bongo_cat_model_behavior_cache_store(app, previous_entry);
    } else if (app->loaded_model[0]) {
        app->behavior_cache_valid = false;
    }
    app->behaviors = *behaviors;
    app->behavior_catalog_valid = behavior_catalog_valid;
    free(behaviors);
    optional = (BongoCatError){0};
    if (bongo_cat_overlay_load(app->overlay, entry->adapter_directory,
        model_pointer, &render_options, &optional) != BONGO_CAT_OK) {
        if (optional.message[0])
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s", optional.message);
        bongo_cat_overlay_clear(app->overlay);
    }
    snprintf(app->loaded_model, sizeof(app->loaded_model), "%s", entry->id);
    size_t saved_behaviors = bongo_cat_app_saved_behavior_count(app, entry->id);
    bongo_cat_app_restore_behavior_state(app, entry->id);
    SDL_Log("[runtime] Behavior restore summary: model=%s saved=%zu "
        "motions=%zu expression=%d", entry->id, saved_behaviors,
        bongo_cat_app_selected_motion_count(app),
        bongo_cat_live2d_expression(app->live2d));
    bongo_cat_model_cover_schedule(app, entry);
    bongo_cat_random_expression_reset(app);
    bongo_cat_random_motion_reset(app);
    bongo_cat_random_sound_reset(app);
    bongo_cat_sequential_model_reset(app);
    app->pointer_known = false;
    bool geometry_changed = bongo_cat_model_apply_aspect(app, &render_options,
        &content_anchor, replacing_model);
    if (app->window) {
        if (geometry_changed) SDL_SyncWindow(app->window);
        SDL_GetWindowSizeInPixels(app->window, &pixel_width, &pixel_height);
    }
    bongo_cat_live2d_resize(app->live2d, pixel_width, pixel_height);
    /* 音频与声音快捷键状态属于上一个模型：新模型可能没有对应音效，而且切换时按住的键不会
       再有按下与抬起事件，不清零就会一直响着。 */
    bongo_cat_audio_reset(app->audio);
    memset(&app->sound_shortcut_state, 0, sizeof(app->sound_shortcut_state));
    memset(app->sound_shortcut_active, 0, sizeof(app->sound_shortcut_active));
    commit_model(app, entry, true, replacing_model);
    bongo_cat_app_reapply_input(app);
    bongo_cat_app_apply_mouse(app);
    SDL_Log("[runtime] Model cover load phase: id=%s phase=%s",
        entry->id, replacing_model ? "switch" : "startup");
    /* Finish the handoff before the independent readback so startup and model
       switches both capture the restored state, never the initial state. */
    app->loading_model[0] = '\0';
    app->model_load_runtime_stage = 0;
    bongo_cat_app_capture_pending_model_cover(app);
    /* Do not leave the previous frame cropped during the UI completion pass. */
    if (replacing_model) bongo_cat_app_render_now(app);
    if (restore_context && previous_window && previous_context &&
        !SDL_GL_MakeCurrent(previous_window, previous_context))
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Cannot restore the previous OpenGL context: %s", SDL_GetError());
    bongo_cat_memory_policy_model_loaded();
    SDL_Log("[runtime] Model switch transaction: stage=complete previous=%s "
        "next=%s elapsed_ms=%.2f current_window=%p current_context=%p "
        "gl_error=0x%x", previous_model, entry->id,
        (double)(SDL_GetTicksNS() - load_started_ns) / 1000000.0,
        (void *)SDL_GL_GetCurrentWindow(), (void *)SDL_GL_GetCurrentContext(),
        gl_backend ? (unsigned)glGetError() : 0u);
    model_runtime_stage(app, "completed", entry);
    model_running_stage(app, entry->id);
    return true;
}

bool bongo_cat_app_select_model(BongoCatApp *app, const char *id) {
    return bongo_cat_app_select_model_with_error(app, id, NULL);
}
