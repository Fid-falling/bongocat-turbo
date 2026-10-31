/* 模型选择与加载的事务实现：从 id 到屏幕上真的换了模型的全部副作用都在这里完成
 * （行为目录、渲染参数、叠加层、几何、音频、封面、内存策略）。
 *
 * 一次切换被切成三段，理由是中间那段（真正加载模型）要 1~5 秒，而主循环必须在这期间持续
 * 出帧，所以它不能和另外两段挤在同一个调用里：
 *   1. bongo_cat_app_begin_model_switch  —— 影子准备，只读当前状态；
 *   2. 分步加载                        —— 由 model_switch.c 按帧预算驱动（见那边的说明）；
 *   3. bongo_cat_app_commit_model_switch —— 提交，全部是不可逆的状态改写。
 * 三段之间不变量不变：新模型没提交之前 app->loaded_model / session 一个字节都不许动，
 * 所以任何一步失败时用户看到的是"没换成"，而不是空白或半加载。
 *
 * 启动路径（bongo_cat_app_select_model_with_error）仍然按顺序把三段跑完，它不需要出帧。
 */
#include "runtime.h"
#include "model_switch.h"
#include "model_import.h"
#include "model_behavior_cache.h"
#include "model_cover.h"
#include "model_geometry.h"
#include "model_switch_shadow.h"
#include "bongo_cat/vk_backend.h"
#include "bongo_cat/audio.h"
#include "bongo_cat/image.h"
#include "bongo_cat/log.h"
#include "bongo_cat/memory_policy.h"
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
    /* 多桌宠时窗口标题 = 模型名: 这里刚把 active_model_id 写进去, 顺手同步一次 */
    bongo_cat_window_sync_model_title(app);
    /* 记住「用过手柄模型」: 手柄子系统的首次初始化要 400~600ms, 只有真的用过它的人才值得在
       下次启动时预热 (见 app.c 里 bongo_cat_gamepads_prewarm 的调用点)。本次的启用照旧在
       下面立刻发生, 所以这里只影响下次启动的行为。 */
    if (entry->mode == BONGO_CAT_MODE_GAMEPAD && !app->session.gamepad_used) {
        app->session.gamepad_used = true;
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[input] 记录「用过手柄模型」: 下次启动会预热手柄子系统");
    }
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

/* 分步加载的进度回调。它和修复前那条同名回调的关键区别是: 现在它只在**步与步之间**被调用,
 * 主循环不需要在这里插帧, 所以它只做两件事 —— 上报运行时阶段, 以及把进度转给设置窗口画
 * "正在切换"的中间态 (bongo_cat_preferences_model_load_progress 内部按 30Hz 限流重绘,
 * 不会把这一帧吃掉)。
 *
 * 历史坑 (别退回去): 旧实现让这个回调自己去 SDL_PumpEvents + bongo_cat_app_render_now
 * 出帧。那是一条"在同步加载里嵌套整帧渲染"的路径, 既没有帧预算, 又会在 GL/Vulkan 两条
 * 后端上各自留下一堆上下文与在飞帧的假设。现在出帧由主循环负责。
 */
void bongo_cat_app_model_load_progress(void *userdata, float progress) {
    BongoCatApp *app = userdata;
    if (!app) return;
    model_progress_runtime_stage(app, progress);
    app->model_load_progress = progress;
    if (app->preferences)
        bongo_cat_preferences_model_load_progress(app->preferences, progress);
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

/* 切换的"影子准备"入口: 校验 id 与前置条件, 再把准备工作交给 model_switch_shadow.c。
 * 已经加载同一个 id 时只补一次 commit（模式可能变过）, 不重新加载也不建影子。 */
bool bongo_cat_app_begin_model_switch(BongoCatApp *app,
    const BongoCatModelEntry *entry, BongoCatModelSwitchShadow *shadow,
    BongoCatError *error) {
    if (!app || !app->live2d || !entry || !shadow) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_ARGUMENT,
            "Cannot select a model without an active renderer and model id");
        return false;
    }
    if (app->loaded_model[0] && strcmp(app->loaded_model, entry->id) == 0) {
        commit_model(app, entry, false, false);
        return false;
    }
    if (!bongo_cat_model_switch_prepare(app, entry, shadow, error)) return false;
    model_runtime_stage(app, "started", entry);
    snprintf(app->loading_model, sizeof(app->loading_model), "%s", entry->id);
    app->model_load_runtime_stage = 0;
    app->model_load_progress = 0.0f;
    return true;
}

/* "要切换的正好就是当前已经加载的模型"这条短路的收尾, 与 begin_model_switch 里那一支完全一致:
 * 只补一次可能变过的模式提交, 不重新加载, 也**不碰影子**。
 *
 * 为什么必须单独开一个函数: 异步队列 (model_switch.c) 也要处理这条路径, 但它走到那里时手上
 * 没有可用的影子 —— 影子要么从未准备过, 要么刚被 job_clear 释放掉 (behaviors 已经是 NULL),
 * 而 bongo_cat_app_commit_model_switch 会无条件解引用 shadow->behaviors。用户在设置页点当前
 * 已选中的模型、在右键菜单里选当前模型、或连点若干次后又绕回原模型, 都会命中这里。 */
bool bongo_cat_app_recommit_loaded_model(BongoCatApp *app,
    const BongoCatModelEntry *entry) {
    if (!app || !entry || !app->loaded_model[0] ||
        strcmp(app->loaded_model, entry->id) != 0) return false;
    commit_model(app, entry, false, false);
    return true;
}

/* 提交: 走到这里说明新模型已经完整可用 (分步加载返回 DONE), 于是按固定顺序改写全部状态。
 * shadow->behaviors 的所有权在这里交出去, 之后不能再调 model_switch_release 释放它。 */
bool bongo_cat_app_commit_model_switch(BongoCatApp *app,
    const BongoCatModelEntry *entry, BongoCatModelSwitchShadow *shadow,
    BongoCatError *error) {
    (void)error;
    if (!app || !entry || !shadow) return false;
    /* 影子必须已经由 model_switch_prepare 填过。空影子走到这里说明调用方用错了入口
       (那条路径是 bongo_cat_app_recommit_loaded_model): 下面无条件解引用 shadow->behaviors,
       与其让主循环直接空指针崩掉, 不如明确报错并拒绝提交 —— 当前模型从始至终没被改过,
       所以拒绝提交的后果只是"没换成", 而不是坏状态。 */
    if (!shadow->behaviors) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Model switch commit called without a prepared shadow: id=%s "
            "(the already-loaded case must use "
            "bongo_cat_app_recommit_loaded_model)", entry->id);
        bongo_cat_error_set(error, BONGO_CAT_ERROR_ARGUMENT,
            "Model switch commit requires a prepared shadow");
        return false;
    }
    const bool replacing_model = shadow->replacing;
    BongoCatLive2DRenderOptions render_options = shadow->render_options;
    BongoCatModelContentAnchor content_anchor = shadow->content_anchor;
    int pixel_width = shadow->pixel_width, pixel_height = shadow->pixel_height;
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
    app->behaviors = *shadow->behaviors;
    app->behavior_catalog_valid = shadow->behavior_catalog_valid;
    /* 所有权交接: 释放容器本身, 但不再走 model_switch_release 的那条 free。 */
    free(shadow->behaviors);
    shadow->behaviors = NULL;
    BongoCatError optional = {0};
    if (bongo_cat_overlay_load(app->overlay, entry->adapter_directory,
        model_pointer, &render_options, &optional) != BONGO_CAT_OK) {
        if (optional.message[0])
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s", optional.message);
        bongo_cat_overlay_clear(app->overlay);
    }
    /* 把设置里的多键显示开关/上限与"气泡合并"模式下发给叠加层。overlay 创建时的默认值
       (开/5/合并) 与设置默认一致, 但用户改过设置之后换模型必须重新下发, 否则新模型仍按旧值画。 */
    bongo_cat_overlay_set_key_display(app->overlay,
        app->settings.model.multi_key_display != 0,
        app->settings.model.multi_key_limit);
    bongo_cat_overlay_set_bubble_merge(app->overlay,
        app->settings.model.bubble_merge != 0);
    snprintf(app->loaded_model, sizeof(app->loaded_model), "%s", entry->id);
    /*
     * 切换完成即作废 Vulkan 里上一模型残留的整帧回读, 让封面捕获的 take 只能等到
     * 新模型渲染完的那一帧 (见 vk_readback.c 的 discard_pending_frame_sample)。
     * OpenGL 路径是同步 glReadPixels, 不存在这个时序窗口, 无需处理。
     */
    if (app->vk)
        bongo_cat_vk_discard_pending_frame_sample(app->vk);
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
    app->loading_model[0] = '\0';
    app->model_load_runtime_stage = 0;
    app->model_load_progress = 0.0f;
    model_running_stage(app, entry->id);
    return true;
}

/* 一次性切换 (启动、目录修复、导入、删除这些"没有画面要维持"的路径用它)。
 * 返回 bool，失败信息写进 *error（传 NULL 时写进内部局部变量，调用方可以不关心）。
 * 前置条件：app 与渲染器已就绪，id 必须存在于 app->models。已经加载同一个 id 时只补一次
 * commit（模式可能变过），不重新加载。失败返回后 app->models 与 session 不变，live2d 也
 * 保持原样 —— 分步加载在提交之前绝不碰 runtime->model。 */
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
    BongoCatModelSwitchShadow shadow;
    if (!bongo_cat_app_begin_model_switch(app, entry, &shadow, failure))
        /* 已经加载的就是这个模型: begin 只补了一次 commit, 算成功; 其余情况是真失败。 */
        return app->loaded_model[0] && strcmp(app->loaded_model, entry->id) == 0;
    char previous_model[BONGO_CAT_ID_CAP];
    snprintf(previous_model, sizeof(previous_model), "%s",
        shadow.replacing ? app->loaded_model : "none");
    uint64_t load_started_ns = SDL_GetTicksNS();
    BongoCatLive2DLoadSession *session = bongo_cat_live2d_load_session_begin(
        app->live2d, entry->directory, entry->setting_file, entry->preset,
        &shadow.render_options, bongo_cat_app_model_load_progress, app, failure);
    if (!session) {
        bongo_cat_model_switch_release(app, &shadow);
        app->loading_model[0] = '\0';
        model_running_stage(app, app->loaded_model);
        return false;
    }
    if (!bongo_cat_model_switch_activate(app, &shadow, failure)) {
        bongo_cat_live2d_load_session_abort(session);
        bongo_cat_model_switch_release(app, &shadow);
        app->loading_model[0] = '\0';
        model_running_stage(app, app->loaded_model);
        return false;
    }
    BongoCatLive2DStepResult step = BONGO_CAT_LIVE2D_STEP_WORKING;
    while (step == BONGO_CAT_LIVE2D_STEP_WORKING)
        step = bongo_cat_live2d_load_session_step(session, entry->directory,
            entry->setting_file, failure);
    if (step != BONGO_CAT_LIVE2D_STEP_DONE) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Model switch transaction: "
            "stage=failed previous=%s next=%s elapsed_ms=%.2f error=%s",
            previous_model, entry->id,
            (double)(SDL_GetTicksNS() - load_started_ns) / 1000000.0,
            failure->message);
        bongo_cat_live2d_load_session_abort(session);
        bongo_cat_model_switch_release(app, &shadow);
        app->loading_model[0] = '\0';
        app->model_load_runtime_stage = 0;
        request_model_frame(app, false);
        model_running_stage(app, app->loaded_model);
        return false;
    }
    bongo_cat_live2d_load_session_end(session);
    if (shadow.replacing) app->last_frame_ns = SDL_GetTicksNS();
    const bool replacing_model = shadow.replacing;
    bongo_cat_app_commit_model_switch(app, entry, &shadow, failure);
    bongo_cat_model_switch_release(app, &shadow);
    bongo_cat_app_capture_pending_model_cover(app);
    /* Do not leave the previous frame cropped during the UI completion pass. */
    if (replacing_model) bongo_cat_app_render_now(app);
    bongo_cat_memory_policy_model_loaded();
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[runtime] Model switch transaction: stage=complete previous=%s "
        "next=%s elapsed_ms=%.2f", previous_model, entry->id,
        (double)(SDL_GetTicksNS() - load_started_ns) / 1000000.0);
    model_runtime_stage(app, "completed", entry);
    return true;
}

bool bongo_cat_app_select_model(BongoCatApp *app, const char *id) {
    return bongo_cat_app_select_model_with_error(app, id, NULL);
}
