/*
 * 模型切换的串行队列与逐帧推进。
 *
 * 用户反馈的现象是"切换桌宠时整个程序阻塞: 一帧都不出、设置界面点不动"。根因是切换在
 * 主线程上同步跑完整段加载 (解析 model3.json / 读 moc3 / 逐个读动作 / 逐个解码上传贴图 /
 * 重建渲染器), 期间主循环完全停摆。修复思路不是"把加载丢到后台线程" (贴图上传用的是当前
 * GL 上下文或后端自己的 Vulkan 命令池, 两者都没有跨线程同步, 加线程只会换来新的竞态),
 * 而是把同一件事切成很多小步, 由主循环按时间预算推进 —— 两次推进之间照常出帧、照常收输入。
 *
 * 本文件负责三件事:
 *   1. 串行化: 任何时刻只有一个切换在跑。用户在设置页连点好几个模型时, 后来的请求**顶掉**
 *      前一个 (先干净地丢弃它的半成品), 于是最终只会应用最后一次选择。
 *   2. 帧预算: 每次推进最多占用 MODEL_SWITCH_FRAME_BUDGET_NS, 到点就返回, 把这一帧剩下的
 *      时间留给渲染与输入。
 *   3. 失败回退: 加载失败或超时 -> 丢弃半成品 (当前模型从始至终没被碰过) -> 上报失败原因,
 *      界面回到原模型, 不出现空白或半加载。
 *
 * 硬约束 (改这里之前先读):
 *   - 所有步骤都必须在主线程执行, 而且必须在"上一帧已经提交完、下一帧还没开始录制"的位置
 *     调用 (app_loop.c 里放在渲染之前), 否则 Vulkan 的队列级等空会撞上在飞的帧;
 *   - app->model_switch 里的 session 只由本文件创建与销毁: 成功走 session_end, 失败/被顶掉
 *     走 session_abort;
 *   - 提交之前不许改 app->loaded_model / session.active_model_id, 这是"失败回退"能成立的
 *     唯一依据。
 */
#include "runtime.h"
#include "model_switch.h"
#include "model_switch_shadow.h"
#include "bongo_cat/log.h"
#include "bongo_cat/memory_policy.h"
#include "bongo_cat/preferences.h"
#include "preferences_notice.h"
#include "preferences_state.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

/*
 * 每帧最多花在加载上的时间。
 *
 * 先看清楚"帧间隔上界"是怎么来的: 主循环先看预算、再做一步、做完再回来看预算, 所以
 *   最大帧间隔 ≈ 预算 + 单步最长耗时
 * 其中"单步最长耗时"由 SDK 内部一次性调用决定 (Vulkan 下 finalize 那一步里的
 * CreateRenderer + CreateCommandBuffer/VertexBuffer/IndexBuffer 合计约 200~270ms, 那是
 * 近 500 条管线与遮罩帧缓冲的创建, 从外面切不开), 所以再往下压预算的收益很小。
 * 取 8ms 而不是更大值是为了"一帧里最多多做一步": 预算越大, 越容易在一步回来之后又踩进
 * 下一步, 白白把这一帧拖长。
 */
#define MODEL_SWITCH_FRAME_BUDGET_NS 8000000ull
/* 单次切换的总超时。到了就判失败并回退原模型: 与其让用户无限期看着"正在切换", 不如明确
 * 告诉他没换成 (日志里还有失败原因)。 */
#define MODEL_SWITCH_TIMEOUT_NS 30000000000ull

/* 把当前这个 job 停掉并清理: 丢弃半成品、还原上下文、清掉 loading_model 这类"仅加载期
 * 有效"的显示状态。它**不**碰 loaded_model/session —— 那两样只在提交时才变。 */
static void job_clear(BongoCatApp *app) {
    if (!app) return;
    if (app->model_switch.session) {
        bongo_cat_live2d_load_session_abort(app->model_switch.session);
        app->model_switch.session = NULL;
    }
    bongo_cat_model_switch_release(app, &app->model_switch.shadow);
    app->model_switch.active = false;
    app->model_switch.target_id[0] = '\0';
    app->model_switch.deadline_ns = 0;
    app->model_switch.started_ns = 0;
    app->model_switch.slowest_step_ns = 0;
    app->model_switch.steps_taken = 0;
    app->model_switch.step_calls = 0;
    app->loading_model[0] = '\0';
    app->model_load_runtime_stage = 0;
    app->model_load_progress = 0.0f;
}

bool bongo_cat_app_model_switch_active(const BongoCatApp *app) {
    return app && app->model_switch.active;
}

const char *bongo_cat_app_model_switch_target(const BongoCatApp *app) {
    return app ? app->model_switch.target_id : "";
}

void bongo_cat_app_model_switch_failure(const BongoCatApp *app,
    BongoCatError *error) {
    if (!error) return;
    *error = app ? app->model_switch.error : (BongoCatError){0};
}

/* 请求切换。它只登记意图并保证"同一时刻只有一个", 不做任何加载: 真正的工作在
 * bongo_cat_app_model_switch_step 里按帧预算推进。
 *
 * 语义上等价于"用户最后点的那一个模型一定会被应用": 已有切换在跑时, 新请求直接把旧的顶掉
 * (旧请求连错误提示都不报, 因为它已经被用户自己撤销了)。 */
bool bongo_cat_app_request_model_switch(BongoCatApp *app, const char *id,
    BongoCatError *error) {
    if (error) *error = (BongoCatError){0};
    if (!app || !app->live2d || !id || !id[0]) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_ARGUMENT,
            "Cannot select a model without an active renderer and model id");
        return false;
    }
    if (!bongo_cat_models_find(&app->models, id)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_ARGUMENT,
            "Model is not installed: %s", id);
        return false;
    }
    if (app->model_switch.active) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[runtime] Model switch superseded: previous=%s next=%s",
            app->model_switch.target_id, id);
        job_clear(app);
    }
    snprintf(app->model_switch.target_id,
        sizeof(app->model_switch.target_id), "%s", id);
    app->model_switch.active = true;
    app->model_switch.deadline_ns = 0;
    app->model_switch.started_ns = 0;
    app->model_switch.error = (BongoCatError){0};
    /* 让"点完立刻看到反馈"成立: 设置页的加载动画由偏好状态驱动, 这里把窗口标脏。 */
    app->dirty = true;
    if (app->preferences) bongo_cat_preferences_invalidate(app->preferences);
    return true;
}

/* 贴图档位升级: 把**当前已加载的**模型按现在的图集上限重新加载一遍。
 * 为什么复用切换队列而不是另开一条路径: 加载的每一步都只能在主线程、按帧预算推进, 而且
 * 提交之前不许动 loaded_model —— 这些不变量在队列里已经成立, 重写一遍只会多一份会走样的
 * 副本。唯一的差别是允许 target 等于 loaded_model (force_reload)。 */
bool bongo_cat_app_request_model_reload(BongoCatApp *app) {
    if (!app || !app->loaded_model[0]) return false;
    app->model_switch.force_reload = true;
    if (bongo_cat_app_request_model_switch(app, app->loaded_model, NULL))
        return true;
    app->model_switch.force_reload = false;
    return false;
}

/* 失败收尾: 记录原因、通知界面、回到原模型。app->loaded_model 与 session 从来没被改过,
 * 所以"回退"这件事本身不需要额外动作 —— 要做的只是把仅加载期有效的显示状态清干净, 并让
 * 主循环重新出帧。 */
static void job_fail(BongoCatApp *app, const char *reason) {
    char failed[BONGO_CAT_ID_CAP];
    snprintf(failed, sizeof(failed), "%s", app->model_switch.target_id);
    if (!app->model_switch.error.message[0] && reason)
        bongo_cat_error_set(&app->model_switch.error, BONGO_CAT_ERROR_CUBISM,
            "%s", reason);
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
        "[runtime] Model switch failed and rolled back: previous=%s next=%s "
        "elapsed_ms=%.2f error=%s",
        app->loaded_model[0] ? app->loaded_model : "none", failed,
        (double)(SDL_GetTicksNS() - app->model_switch.started_ns) / 1000000.0,
        app->model_switch.error.message[0] ? app->model_switch.error.message
            : "unknown error");
    job_clear(app);
    /* 提示失败原因: 文案由 UI 层从 i18n 表里取 (10 种语言), 这里只负责把错误传过去。 */
    if (app->preferences)
        bongo_cat_preferences_model_switch_failed(app->preferences, failed,
            app->model_switch.error.message);
    app->dirty = true;
}

/* 成功收尾: 把影子交给提交那一段, 再走与一次性版本相同的收尾顺序 (封面/补一帧/内存策略)。
 * 顺序不能变: 封面必须在 loaded_model 已经指向新模型之后安排, 补画那一帧必须在提交之后。 */
static void job_complete(BongoCatApp *app, const BongoCatModelEntry *entry) {
    const bool replacing_model = app->model_switch.shadow.replacing;
    const uint64_t started_ns = app->model_switch.started_ns;
    const double slowest_ms =
        (double)app->model_switch.slowest_step_ns / 1000000.0;
    const uint64_t step_calls = app->model_switch.step_calls;
    const uint64_t steps_taken = app->model_switch.steps_taken;
    char previous[BONGO_CAT_ID_CAP];
    snprintf(previous, sizeof(previous), "%s",
        replacing_model ? app->loaded_model : "none");
    if (replacing_model) app->last_frame_ns = SDL_GetTicksNS();
    BongoCatError failure = {0};
    bongo_cat_app_commit_model_switch(app, entry, &app->model_switch.shadow,
        &failure);
    /* 提交已经把 behaviors 的所有权接走, 这里的 release 只负责还原 GL 上下文。 */
    bongo_cat_model_switch_release(app, &app->model_switch.shadow);
    app->model_switch.active = false;
    app->model_switch.target_id[0] = '\0';
    app->model_switch.session = NULL;
    app->model_switch.slowest_step_ns = 0;
    /*
     * 封面不在这里急切抓取。提交瞬间 loaded_model 已指向新模型, 但新模型还没被渲染过;
     * Vulkan 的整帧回读有 ≥1 帧延迟, 此刻抓到的仍是旧模型的帧 —— 会把旧画面写进新模型
     * 的 cover.png。抓取交给主循环的 bongo_cat_app_capture_pending_model_cover():
     * 它自己会先渲染一帧新模型 (present=false) 再 take, 天然满足"先画新模型、再抓、再写"。
     */
    /* 不要给用户留下一帧被裁过的旧画面: UI 收尾阶段再补一帧。 */
    if (replacing_model) bongo_cat_app_render_now(app);
    bongo_cat_memory_policy_model_loaded();
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[runtime] Model switch transaction: "
        "stage=complete previous=%s next=%s elapsed_ms=%.2f "
        "slowest_step_ms=%.2f step_calls=%llu steps=%llu", previous, entry->id,
        (double)(SDL_GetTicksNS() - started_ns) / 1000000.0, slowest_ms,
        (unsigned long long)step_calls, (unsigned long long)steps_taken);
    if (app->preferences)
        bongo_cat_preferences_model_switch_finished(app->preferences, entry->id);
    app->dirty = true;
}

/* 第一次推进: 建影子与加载会话, 并把 GL 上下文切进来。返回 false 表示已经失败收尾。 */
static bool job_start(BongoCatApp *app, const BongoCatModelEntry *entry,
    uint64_t now) {
    app->model_switch.started_ns = now;
    app->model_switch.deadline_ns = now + MODEL_SWITCH_TIMEOUT_NS;
    BongoCatError failure = {0};
    const bool force = app->model_switch.force_reload;
    /* force 只对本次加载有效: 用过就清, 免得后面某次普通切换被它带成"重载同一个模型"。 */
    app->model_switch.force_reload = false;
    if (!bongo_cat_app_begin_model_switch(app, entry,
            &app->model_switch.shadow, force, &failure)) {
        /* begin 的两种失败: 参数不对 (前面已经挡过) 或"已经加载的就是它" (上层挡过), 所以
         * 走到这里只剩真失败。 */
        app->model_switch.error = failure;
        job_fail(app, NULL);
        return false;
    }
    app->model_switch.session = bongo_cat_live2d_load_session_begin(app->live2d,
        entry->directory, entry->setting_file, entry->preset,
        &app->model_switch.shadow.render_options,
        bongo_cat_app_model_load_progress, app, &app->model_switch.error);
    if (!app->model_switch.session) {
        job_fail(app, NULL);
        return false;
    }
    if (!bongo_cat_model_switch_activate(app, &app->model_switch.shadow,
            &app->model_switch.error)) {
        job_fail(app, NULL);
        return false;
    }
    return true;
}

bool bongo_cat_app_model_switch_step(BongoCatApp *app, uint64_t now,
    uint64_t budget_ns) {
    if (!app || !app->model_switch.active) return false;
    const BongoCatModelEntry *entry = bongo_cat_models_find(&app->models,
        app->model_switch.target_id);
    if (!entry) {
        bongo_cat_error_set(&app->model_switch.error, BONGO_CAT_ERROR_ARGUMENT,
            "Model is not installed: %s", app->model_switch.target_id);
        job_fail(app, NULL);
        return true;
    }
    /* 已经加载的就是这个模型: 只补一次 commit (模式可能变过), 不重新加载。
     *
     * 这里**不能**走 bongo_cat_app_commit_model_switch: 那个函数要求影子已经被
     * bongo_cat_app_begin_model_switch 填好 (它会无条件解引用 shadow->behaviors), 而走到这个
     * 分支时影子要么从未准备, 要么刚被 job_clear 释放成 NULL —— 完整提交会在主循环里空指针
     * 崩溃 (实测 0xC0000005, 帧序列直接断掉)。用户在设置页点当前已选中的模型、在右键菜单里选
     * 当前模型、或连点若干次后又绕回原模型, 都会命中这里, 所以它是一条常见路径而不是边角。 */
    if (app->loaded_model[0] && strcmp(app->loaded_model, entry->id) == 0) {
        bongo_cat_app_recommit_loaded_model(app, entry);
        bongo_cat_model_switch_release(app, &app->model_switch.shadow);
        app->model_switch.active = false;
        app->model_switch.target_id[0] = '\0';
        app->loading_model[0] = '\0';
        app->model_load_runtime_stage = 0;
        app->model_load_progress = 0.0f;
        /* 这行是"绕回原模型时没有重新加载"的证据: 它只会出现在这条分支上。 */
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[runtime] Model switch already loaded: id=%s skipped_reload=1",
            entry->id);
        if (app->preferences)
            bongo_cat_preferences_model_switch_finished(app->preferences,
                entry->id);
        app->dirty = true;
        return true;
    }
    if (!app->model_switch.started_ns && !job_start(app, entry, now))
        return true;
    if (now >= app->model_switch.deadline_ns) {
        bongo_cat_error_set(&app->model_switch.error, BONGO_CAT_ERROR_IO,
            "Model loading timed out after %.0f seconds",
            (double)MODEL_SWITCH_TIMEOUT_NS / 1000000000.0);
        job_fail(app, NULL);
        return true;
    }
    if (!budget_ns) budget_ns = MODEL_SWITCH_FRAME_BUDGET_NS;
    app->model_switch.step_calls++;
    /*
     * 关键: 每做完一步都**重新**检查预算, 而不是"进入前检查一次、然后一直做到超时"。
     * 后者的问题是一步可能远超预算 (解码一张大贴图在 Vulkan 下要 150~200ms), 于是"预算
     * 50ms + 一步 200ms"能连做几步, 一帧就拖到 500ms 以上 —— 这正是实测里帧间隔没降下来
     * 的原因。改成"先看预算, 再做一步, 再看预算"之后, 一帧最多只多花一步的时间:
     *   最大帧间隔 ≈ 预算 + 单步最长耗时
     * 这就是修复期间帧间隔的上界, 也是为什么单步粒度 (而不是预算大小) 才是真正的瓶颈。
     */
    const uint64_t deadline = now + budget_ns;
    while (SDL_GetTicksNS() < deadline) {
        const uint64_t step_started = SDL_GetTicksNS();
        app->model_switch.steps_taken++;
        BongoCatLive2DStepResult step = bongo_cat_live2d_load_session_step(
            app->model_switch.session, entry->directory, entry->setting_file,
            &app->model_switch.error);
        /* 记录"单独一步有多长": 切换期间的帧间隔上界就是"预算 + 这一步的长度", 而单步长度
         * 由各阶段决定 —— 收尾日志里的 slowest_step_ms 与 stage timing 那一行就是结论。 */
        const uint64_t step_ns = SDL_GetTicksNS() - step_started;
        if (step_ns > app->model_switch.slowest_step_ns)
            app->model_switch.slowest_step_ns = step_ns;
        if (step == BONGO_CAT_LIVE2D_STEP_DONE) {
            /* 分段计时必须在 session_end 之前取: 摘要存在会话对象里, 会话一释放就没了。 */
            bongo_cat_live2d_load_session_log_timing(app->model_switch.session,
                entry->id, SDL_GetTicksNS() - app->model_switch.started_ns);
            bongo_cat_live2d_load_session_end(app->model_switch.session);
            app->model_switch.session = NULL;
            job_complete(app, entry);
            return true;
        }
        if (step == BONGO_CAT_LIVE2D_STEP_FAILED) {
            job_fail(app, NULL);
            return true;
        }
    }
    /* 这一帧的预算用完了, 但切换还在进行: 标脏让主循环继续出帧 (进度条/中间态要动)。 */
    app->dirty = true;
    if (app->preferences) bongo_cat_preferences_invalidate(app->preferences);
    return true;
}
