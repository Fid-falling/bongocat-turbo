/*
 * 可分步的 Live2D 模型加载 (cubism_runtime.hpp 里 LoadCursor 的调度实现)。
 *
 * 它和 cubism_bridge_load.cpp 的一次性版本做的是同一件事, 区别只在"谁来切时间": 一次性版本
 * 一口气跑完全部步骤 (启动路径用它, 那时没有画面要维持), 这里则由主循环按时间预算一步一
 * 步推进。两条路都遵守同一条不变量 —— **新模型在 commit 之前绝不写 runtime->model**, 所以
 * 加载中途失败时旧模型仍然完好, 用户看到的是"没换成", 而不是黑屏或半加载。
 *
 * 上下文处理与一次性版本一致: 只有 OpenGL 后端需要临时切到主窗口的上下文 (Vulkan 没有 GL
 * 上下文), 而且 begin 记下的窗口/上下文必须在 finish 与 abort 两条出口都还原。分步之后这件事
 * 变成跨调用的状态, 所以存在 session 里而不是栈上。
 *
 * 线程约定: 全部步骤都必须在**主线程**上执行, 因为贴图上传走的是后端自己的命令池 (Vulkan)
 * 或当前 GL 上下文 (OpenGL), 两者都没有跨线程同步。这里刻意不引入工作线程 —— 需要的是
 * "主线程别被占住", 而分步已经做到了, 再加线程只会引入新的同步面。
 */
#include "cubism_runtime.hpp"

#include "bongo_cat/model.h"
#include "bongo_cat/render.h"

#include <SDL3/SDL_log.h>
#include <SDL3/SDL_opengl.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include <cstdio>
#include <new>

/* 一次可分步加载的全部跨调用状态。定义只属于本文件 —— C 侧只拿到不透明句柄。
 * 上下文存成 SDL_GLContext 而不是 void*, 因为 SDL_GL_MakeCurrent 不接受 void*;
 * main_* 是"应该用哪个上下文加载", previous_* 是"进来之前当前的是哪个" (退出时还原)。 */
struct BongoCatLive2DLoadSession {
    BongoCatLive2D *runtime = nullptr;
    bongo_cat::NativeModel *pending = nullptr;
    LoadCursor cursor = LoadCursor::kSettingsRead;
    int texture_index = 0;
    float progress = 0.0f;
    bool direct_textures = false;
    bool context_switched = false;
    SDL_Window *main_window = nullptr;
    SDL_GLContext main_context = nullptr;
    SDL_Window *previous_window = nullptr;
    SDL_GLContext previous_context = nullptr;
    /* 进度回调由上层提供 (设置窗口靠它画"正在切换"的中间态), 只在步与步之间触发。 */
    BongoCatLive2DLoadProgress progress_callback = nullptr;
    void *progress_userdata = nullptr;
    /*
     * 按游标累计的耗时/次数/单步峰值。存在的理由是"单步最长耗时"直接决定切换期间的帧间隔
     * 上界 (预算 + 单步峰值), 所以优化时先看这里, 而不是猜哪一段慢。
     * stage_ns 的寿命只到加载结束 —— 由 session_end/abort 连会话一起丢掉。
     */
    uint64_t stage_ns[kLoadStageCount] = {};
    uint64_t stage_max_ns[kLoadStageCount] = {};
    unsigned stage_steps[kLoadStageCount] = {};
};

namespace {

/* 分步进度映射: 与一次性版本 bongo_cat_live2d_load 的档位保持一致, 这样设置窗口那条
 * 两段式动画曲线的输入语义不变。 */
float settings_step_progress(int texture_index, int texture_total) {
    if (texture_total <= 0) return 1.0f;
    return .50f + .45f * (float)texture_index / (float)texture_total;
}

} // namespace

const char *bongo_cat_live2d_load_stage_name(LoadCursor cursor) {
    switch (cursor) {
    case LoadCursor::kSettingsRead: return "settings-read";
    case LoadCursor::kSettingsApply: return "settings-apply";
    case LoadCursor::kMotions: return "motions";
    case LoadCursor::kTexturesBegin: return "textures-begin";
    case LoadCursor::kTextures: return "textures-upload";
    case LoadCursor::kTexturesMask: return "textures-mask";
    case LoadCursor::kFinalize: return "finalize-renderer";
    case LoadCursor::kDone: return "done";
    case LoadCursor::kFailed: return "failed";
    }
    return "unknown";
}

/*
 * 把这次加载里"哪一段最慢"打出来。切换期间的帧间隔上界是"每帧预算 + 单步最长耗时", 所以
 * 用户反馈"切换还是有点卡"时, 这一行直接指出该优化谁 —— 而不是靠猜。
 * 必须在会话还活着的时候调 (也就是 session_end/abort 之前); 只在单步峰值超过 32ms 时打,
 * 低于它就与一帧的时间同阶, 不值得为它做优化。
 */
extern "C" void bongo_cat_live2d_load_session_log_timing(
    const BongoCatLive2DLoadSession *session, const char *model_id,
    uint64_t total_ns) {
    if (!session) return;
    uint64_t slowest = 0;
    for (unsigned i = 0; i < kLoadStageCount; ++i)
        if (session->stage_max_ns[i] > slowest) slowest = session->stage_max_ns[i];
    if (slowest < 32000000ull) return;
    char detail[384];
    size_t used = 0;
    detail[0] = '\0';
    for (unsigned i = 0; i < kLoadStageCount && used + 48 < sizeof(detail); ++i) {
        if (!session->stage_steps[i]) continue;
        int written = snprintf(detail + used, sizeof(detail) - used,
            "%s%s=%.1fms/%u(max %.1f)",
            used ? " " : "",
            bongo_cat_live2d_load_stage_name((LoadCursor)i),
            (double)session->stage_ns[i] / 1000000.0, session->stage_steps[i],
            (double)session->stage_max_ns[i] / 1000000.0);
        if (written <= 0) break;
        used += (size_t)written;
    }
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
        "[runtime] Live2D load stage timing: model=%s total=%.1fms peak=%.1fms %s",
        model_id ? model_id : "", (double)total_ns / 1000000.0,
        (double)slowest / 1000000.0, detail);
}

void bongo_cat_live2d_session_report(BongoCatLive2DLoadSession *session,
    float progress) {
    if (!session || !session->progress_callback) return;
    if (progress < session->progress) progress = session->progress;
    if (progress == session->progress && session->progress > 0.0f) return;
    session->progress = progress;
    session->progress_callback(session->progress_userdata, session->progress);
}

/* 退出分步加载: 释放 pending (它从来没被装到 runtime 上), 再把上下文还原。
 * 幂等 —— abort 之后又走到 finish 也只是重复一次还原。 */
void bongo_cat_live2d_session_discard(BongoCatLive2DLoadSession *session) {
    if (!session) return;
    delete session->pending;
    session->pending = nullptr;
    if (session->context_switched && session->previous_window &&
        session->previous_context &&
        !SDL_GL_MakeCurrent(session->previous_window, session->previous_context))
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Cannot restore the previous OpenGL context after a model load: %s",
            SDL_GetError());
    session->context_switched = false;
}

extern "C" BongoCatLive2DLoadSession *bongo_cat_live2d_load_session_begin(
    BongoCatLive2D *runtime, const char *directory, const char *setting_file,
    bool direct_textures, const BongoCatLive2DRenderOptions *render_options,
    BongoCatLive2DLoadProgress progress, void *userdata, BongoCatError *error) {
    if (!runtime) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_ARGUMENT,
            "Cannot load a Live2D model without a runtime");
        return nullptr;
    }
    auto *session = new(std::nothrow) BongoCatLive2DLoadSession();
    if (!session) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY,
            "Cannot allocate the Live2D load session");
        return nullptr;
    }
    session->runtime = runtime;
    session->direct_textures = direct_textures;
    session->progress_callback = progress;
    session->progress_userdata = userdata;
    session->pending = new(std::nothrow) bongo_cat::NativeModel();
    if (!session->pending) {
        delete session;
        bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY,
            "Cannot allocate Live2D model");
        return nullptr;
    }
    if (render_options) session->pending->set_render_options(*render_options);
    /*
     * 上下文切换与一次性版本同一套判据: Vulkan 后端没有 GL 上下文, 不碰上下文也不读
     * glGetError; OpenGL 后端只有"当前上下文不是主窗口的"时才需要临时切过去。
     */
    session->main_window = SDL_GL_GetCurrentWindow();
    session->main_context = SDL_GL_GetCurrentContext();
    session->previous_window = session->main_window;
    session->previous_context = session->main_context;
    return session;
}

extern "C" BongoCatLive2DStepResult bongo_cat_live2d_load_session_activate(
    BongoCatLive2DLoadSession *session, SDL_Window *window, void *context,
    BongoCatError *error) {
    if (!session) return BONGO_CAT_LIVE2D_STEP_FAILED;
    const bool gl_backend =
        bongo_cat_renderer_active() != BONGO_CAT_RENDERER_VULKAN;
    /* C 侧把 GL 上下文存成 void* (它是平台无关句柄), 这里只是转回来。 */
    SDL_GLContext target = (SDL_GLContext)context;
    session->main_window = window;
    session->main_context = target;
    if (!gl_backend) return BONGO_CAT_LIVE2D_STEP_WORKING;
    if (SDL_GL_GetCurrentWindow() == window &&
        SDL_GL_GetCurrentContext() == target)
        return BONGO_CAT_LIVE2D_STEP_WORKING;
    if (!SDL_GL_MakeCurrent(window, target)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Cannot activate the main OpenGL context: %s", SDL_GetError());
        return BONGO_CAT_LIVE2D_STEP_FAILED;
    }
    session->context_switched = true;
    return BONGO_CAT_LIVE2D_STEP_WORKING;
}

/*
 * 推进一步。每一步都在 **单个** 动作/贴图这个粒度上返回, 由调用方决定这一帧还能不能再走
 * 一步 (见 model_switch.c 的时间预算)。返回值三态: WORKING 继续, DONE 成功 (runtime 已经
 * 换成新模型), FAILED 失败 (error 已填, runtime 一个字节都没动过)。
 */
extern "C" BongoCatLive2DStepResult bongo_cat_live2d_load_session_step(
    BongoCatLive2DLoadSession *session, const char *directory,
    const char *setting_file, BongoCatError *error) {
    if (!session || !session->pending || !session->runtime)
        return BONGO_CAT_LIVE2D_STEP_FAILED;
    bongo_cat::NativeModel *model = session->pending;
    BongoCatLive2D *runtime = session->runtime;
    /*
     * 只做一步就返回, 绝不在内部循环 —— "每帧走几步"由 model_switch.c 的时间预算决定。
     * 这样"一次 step = 一个可计量的工作单元", 切换期间的帧间隔上界就是"预算 + 单步最长
     * 耗时", 而单步最长耗时落在哪一段由 session->stage_ns 直接给出来。
     * 游标推进 (kTextures 满 -> kTexturesMask 之类) 用 continue 走到下一个 case 是允许的:
     * 那只是一次状态跳转, 不产生新的工作。
     */
    const LoadCursor cursor = session->cursor;
    const uint64_t stage_started = SDL_GetTicksNS();
    BongoCatLive2DStepResult outcome = BONGO_CAT_LIVE2D_STEP_WORKING;
    for (;;) {
        switch (session->cursor) {
        case LoadCursor::kSettingsRead:
            if (!model->settings_begin(directory, setting_file, error)) {
                session->cursor = LoadCursor::kFailed;
                outcome = BONGO_CAT_LIVE2D_STEP_FAILED;
                break;
            }
            bongo_cat_live2d_session_report(session, .10f);
            session->cursor = LoadCursor::kSettingsApply;
            break;
        case LoadCursor::kSettingsApply:
            if (!model->settings_finish(error)) {
                session->cursor = LoadCursor::kFailed;
                outcome = BONGO_CAT_LIVE2D_STEP_FAILED;
                break;
            }
            bongo_cat_live2d_session_report(session, .35f);
            model->motions_begin();
            session->cursor = LoadCursor::kMotions;
            break;
        case LoadCursor::kMotions:
            /* 一次一个动作文件: 动作多的大模型 (上百个 .motion3.json) 就靠这里被切碎。 */
            if (model->motions_step())
                bongo_cat_live2d_session_report(session, .49f);
            else {
                bongo_cat_live2d_session_report(session, .49f);
                session->cursor = LoadCursor::kTexturesBegin;
            }
            break;
        case LoadCursor::kTexturesBegin:
            /* reshape 放在这一步而不是 begin: 尺寸要按调用方"现在"的窗口算, 而 begin 可能
             * 在好几帧之前就发生了。 */
            model->reshape(runtime->width, runtime->height);
            model->textures_begin();
            session->texture_index = 0;
            bongo_cat_live2d_session_report(session, .50f);
            session->cursor = LoadCursor::kTextures;
            break;
        case LoadCursor::kTextures: {
            const int total = model->texture_total();
            if (session->texture_index >= total) {
                session->cursor = LoadCursor::kFinalize;
                break;
            }
            /* 记下索引但不前进: 前进交给配对的 kTexturesMask, 这样"上传第 i 张"与
             * "给第 i 张补掩码"之间不会夹进别的上传。 */
            if (!model->textures_step(session->texture_index, error)) {
                session->cursor = LoadCursor::kFailed;
                outcome = BONGO_CAT_LIVE2D_STEP_FAILED;
                break;
            }
            session->cursor = LoadCursor::kTexturesMask;
            break;
        }
        case LoadCursor::kTexturesMask:
            /*
             * 一张贴图拆成"上传"与"生成 alpha 掩码"两步。掩码是逐像素的纯 CPU 活,
             * 单独成步能把单步耗时压下来 (切换期间的帧间隔上界 = 预算 + 单步最长耗时),
             * 同时让主循环有机会在这两步之间插进一帧。GL 路径的掩码在解码时已填好,
             * 这一步对它就是空操作。
             *
             * 两步必须**交替**走 (上传 i -> 掩码 i -> 上传 i+1), 不能"全部上传完再统一补
             * 掩码": Vulkan 分支的 textures_step 只保留一份待处理位图 (pending_alpha_image_),
             * 下一次上传会把它顶掉, 于是除最后一张外全都拿不到掩码 —— prepare_expression_frame
             * 随后会把整张透明画布当成可见内容, frame 内边距偏大, 模型被画小、内容视口与
             * 叠加层错位。掩码在 textures_begin 里已按贴图数一次性分配, 与这里的索引一一对应。
             */
            model->textures_mask_step(session->texture_index);
            session->texture_index++;
            session->cursor = LoadCursor::kTextures;
            bongo_cat_live2d_session_report(session,
                settings_step_progress(session->texture_index,
                    model->texture_total()));
            break;
        case LoadCursor::kFinalize:
            if (!model->textures_end(error)) {
                session->cursor = LoadCursor::kFailed;
                outcome = BONGO_CAT_LIVE2D_STEP_FAILED;
                break;
            }
            /* 交接点: 走到这里新模型已经完整可用, 才允许替换当前模型并把旧的排进退休
             * 队列 (退休队列由 draw() 按帧推进, 见 cubism_bridge.cpp)。 */
            {
                bongo_cat::NativeModel *previous = runtime->model;
                runtime->model = model;
                session->pending = nullptr;
                if (previous) {
                    if (runtime->retired_count == runtime->retired_capacity) {
                        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                            "Live2D retirement queue reached its limit; releasing the oldest model");
                        delete runtime->retired[0].model;
                        for (unsigned i = 1; i < runtime->retired_count; ++i)
                            runtime->retired[i - 1] = runtime->retired[i];
                        runtime->retired_count--;
                    }
                    BongoCatRetiredModel *slot =
                        &runtime->retired[runtime->retired_count++];
                    slot->model = previous;
                    /* 3 帧: 双缓冲/三缓冲的交换链最多还有两帧在飞, 多留一帧作余量。 */
                    slot->frames_remaining = 3;
                }
            }
            /* 进度回调也在这里 (不是 report): 它必须无条件报到 1.0, report 会去重。 */
            session->progress = 1.0f;
            if (session->progress_callback)
                session->progress_callback(session->progress_userdata, 1.0f);
            session->cursor = LoadCursor::kDone;
            outcome = BONGO_CAT_LIVE2D_STEP_DONE;
            break;
        case LoadCursor::kDone:
            outcome = BONGO_CAT_LIVE2D_STEP_DONE;
            break;
        case LoadCursor::kFailed:
        default:
            outcome = BONGO_CAT_LIVE2D_STEP_FAILED;
            break;
        }
        break;
    }
    const uint64_t elapsed = SDL_GetTicksNS() - stage_started;
    const unsigned slot = (unsigned)cursor;
    if (slot < kLoadStageCount) {
        session->stage_ns[slot] += elapsed;
        session->stage_steps[slot]++;
        if (elapsed > session->stage_max_ns[slot])
            session->stage_max_ns[slot] = elapsed;
    }
    return outcome;
}

/* 加载成功 (或调用方决定放弃) 之后的收尾: 还原上下文并释放会话本身。不释放模型 ——
 * 成功时它已经是 runtime->model, 失败时由 discard 负责。 */
extern "C" void bongo_cat_live2d_load_session_end(
    BongoCatLive2DLoadSession *session) {
    if (!session) return;
    if (session->context_switched && session->previous_window &&
        session->previous_context &&
        !SDL_GL_MakeCurrent(session->previous_window, session->previous_context))
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Cannot restore the previous OpenGL context after a model switch: %s",
            SDL_GetError());
    session->context_switched = false;
    delete session;
}

extern "C" void bongo_cat_live2d_load_session_abort(
    BongoCatLive2DLoadSession *session) {
    if (!session) return;
    bongo_cat_live2d_session_discard(session);
    delete session;
}
