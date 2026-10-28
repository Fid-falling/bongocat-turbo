/*
 * "自动回退 OpenGL" 的持久化记忆。
 *
 * 存哪儿、为什么:
 *   <config_root>/renderer-fallback.json —— 与 settings.json 同目录的一个独立小文件,
 *   **不塞进 settings.json**。理由三条, 与 update-sources.state 的做法一致:
 *   1. 它是这台机器上的运行证据 (连续失败次数/最近原因/时间/显卡), 不是用户偏好。
 *      settings.json 是用户会手改、文档里也写明的偏好 schema; 把会自动增长的诊断计数
 *      混进去, 用户改配置时看到会一头雾水, 也让"读/写必须一一对齐"的配置契约多背一份
 *      无关状态。
 *   2. 它必须在"设置存储可能被 blocked / smoke 不落盘"时也能独立写入 —— 看门狗回退
 *      发生在一个正在卡死的进程里, 这条记录是下一次启动不再踩坑的唯一依据, 不能受配置
 *      存档策略影响。
 *   3. 加字段不需要动 config 的 schema/校验/迁移, 也就不会波及 10 份 locale 之外的
 *      任何配置测试。
 *
 * 规则 (与设置页提示条、自检报告共用同一份内存状态):
 *   - 每次**自动**回退 -> 计数 +1 (首帧超时与 Vulkan 初始化失败都算; 正常启动/手动切 GL
 *     不计);
 *   - 计数达到 BONGO_CAT_RENDERER_FALLBACK_PAUSE_THRESHOLD -> auto_disabled=true, 之后
 *     启动不再自动尝试 Vulkan (直接 OpenGL), 直到用户手动重试;
 *   - 用户手动把后端设为 Vulkan (设置页/托盘/提示条按钮) -> 清零并解除暂停;
 *   - Vulkan 首帧成功呈现 -> 清零并解除暂停;
 *   - 写盘失败时退回"把 settings 写死 OpenGL"的硬保险: 宁可丢掉计数, 也不能让下次启动
 *     再次卡在同一个后端上 (那会变成"卡死 -> 重启 -> 卡死"的循环)。
 *
 * 文件格式 (给支持人员看, 字段名属于诊断契约):
 *   { "version": 1, "consecutiveFailures": 2, "pauseThreshold": 2,
 *     "autoDisabled": true, "lastReason": "first-frame-timeout",
 *     "lastDetail": "...", "lastGpu": "...", "lastFailureAt": "..." }
 */
#include "runtime.h"
#include "renderer_fallback_state.h"
#include "runtime_state.h"

#include "bongo_cat/file.h"
#include "bongo_cat/json.h"
#include "bongo_cat/log.h"
#include "bongo_cat/path.h"

#include <stdio.h>
#include <string.h>

#define FALLBACK_STATE_FILE "renderer-fallback.json"
#define FALLBACK_STATE_TEMPORARY "renderer-fallback.tmp"
/* 状态文件只有十来个短字段; 读 4KB 足够, 再大就是别人放错文件了, 按损坏处理。 */
#define FALLBACK_STATE_READ_CAP 4096

static bool state_path(const BongoCatApp *app, const char *name, char *output,
    size_t capacity) {
    return app && app->config_root[0] &&
        bongo_cat_path_join(output, capacity, app->config_root, name);
}

/* 读一个可选字符串字段; 超长按容量截断 (状态是诊断信息, 截断比读失败好),
   保证输出总是以 '\0' 结尾。 */
static void read_text_field(yyjson_val *root, const char *key, char *output,
    size_t capacity) {
    if (!output || capacity == 0) return;
    output[0] = '\0';
    yyjson_val *value = yyjson_obj_get(root, key);
    if (!yyjson_is_str(value)) return;
    const char *text = yyjson_get_str(value);
    size_t length = yyjson_get_len(value);
    if (!text) return;
    if (length >= capacity) length = capacity - 1;
    memcpy(output, text, length);
    output[length] = '\0';
}

static void state_zero(BongoCatRendererFallbackState *state) {
    memset(state, 0, sizeof(*state));
}

/*
 * 规范化: 没有失败次数的记录等于没有记录 (只有暂停标志而没有计数是自相矛盾的, 只能来自
 * 手改文件或写了一半的状态); 计数达到阈值时暂停标志必须是开着的。这样读取端与写入端
 * 不依赖"文件恰好写对了"。
 */
static void state_normalize(BongoCatRendererFallbackState *state) {
    if (state->consecutive_failures <= 0) {
        state_zero(state);
        return;
    }
    if (state->consecutive_failures > 999) state->consecutive_failures = 999;
    if (state->consecutive_failures >= BONGO_CAT_RENDERER_FALLBACK_PAUSE_THRESHOLD)
        state->auto_disabled = true;
}

/* 原子写: 先写 .tmp 再替换, 断电/被杀不会留下半份 JSON (与 runtime-state.txt 同一套)。 */
static bool state_save(BongoCatApp *app, const BongoCatRendererFallbackState *state) {
    char path[BONGO_CAT_PATH_CAP], temporary[BONGO_CAT_PATH_CAP];
    if (!state_path(app, FALLBACK_STATE_FILE, path, sizeof(path)) ||
        !state_path(app, FALLBACK_STATE_TEMPORARY, temporary,
            sizeof(temporary))) return false;
    yyjson_mut_doc *document = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = document ? yyjson_mut_obj(document) : NULL;
    if (document) yyjson_mut_doc_set_root(document, root);
    bool ok = root &&
        yyjson_mut_obj_add_int(document, root, "version", 1) &&
        yyjson_mut_obj_add_int(document, root, "consecutiveFailures",
            state->consecutive_failures) &&
        yyjson_mut_obj_add_int(document, root, "pauseThreshold",
            BONGO_CAT_RENDERER_FALLBACK_PAUSE_THRESHOLD) &&
        yyjson_mut_obj_add_bool(document, root, "autoDisabled",
            state->auto_disabled) &&
        yyjson_mut_obj_add_strcpy(document, root, "lastReason",
            state->last_reason) &&
        yyjson_mut_obj_add_strcpy(document, root, "lastDetail",
            state->last_detail) &&
        yyjson_mut_obj_add_strcpy(document, root, "lastGpu", state->last_gpu) &&
        yyjson_mut_obj_add_strcpy(document, root, "lastFailureAt",
            state->last_failure_at);
    FILE *file = ok ? bongo_cat_file_open(temporary, "wb") : NULL;
    if (file) {
        yyjson_write_err write_error = {0};
        if (!yyjson_mut_write_fp(file, document, YYJSON_WRITE_PRETTY,
                NULL, &write_error)) ok = false;
        if (fclose(file) != 0) ok = false;
    } else ok = false;
    if (document) yyjson_mut_doc_free(document);
    if (ok && bongo_cat_file_replace(temporary, path, true)) return true;
    bongo_cat_file_remove(temporary);
    return false;
}

void bongo_cat_renderer_fallback_load(BongoCatApp *app) {
    if (!app) return;
    state_zero(&app->renderer_fallback);
    char path[BONGO_CAT_PATH_CAP];
    if (!state_path(app, FALLBACK_STATE_FILE, path, sizeof(path)))
        return;
    if (!bongo_cat_path_is_file(path)) return;
    yyjson_read_err read_error = {0};
    yyjson_doc *document = bongo_cat_json_read_file(path,
        YYJSON_READ_ALLOW_BOM, &read_error);
    yyjson_val *root = document ? yyjson_doc_get_root(document) : NULL;
    if (!yyjson_is_obj(root)) {
        SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
            "[render] 自动回退状态文件无法解析 (%s: %s), 按没有记录处理; "
            "下次回退会覆盖它",
            path, read_error.msg ? read_error.msg : "not a JSON object");
        if (document) yyjson_doc_free(document);
        return;
    }
    BongoCatRendererFallbackState state;
    state_zero(&state);
    state.consecutive_failures =
        yyjson_get_int(yyjson_obj_get(root, "consecutiveFailures"));
    state.auto_disabled = yyjson_get_bool(yyjson_obj_get(root, "autoDisabled"));
    read_text_field(root, "lastReason", state.last_reason,
        sizeof(state.last_reason));
    read_text_field(root, "lastDetail", state.last_detail,
        sizeof(state.last_detail));
    read_text_field(root, "lastGpu", state.last_gpu, sizeof(state.last_gpu));
    read_text_field(root, "lastFailureAt", state.last_failure_at,
        sizeof(state.last_failure_at));
    yyjson_doc_free(document);
    state_normalize(&state);
    app->renderer_fallback = state;
    if (state.consecutive_failures <= 0) return;
    SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
        "[render] 自动回退记录: 连续 %d 次 (%s), 最近一次 %s%s%s; %s",
        state.consecutive_failures,
        bongo_cat_renderer_fallback_reason_label(state.last_reason),
        state.last_failure_at[0] ? state.last_failure_at : "时间未记录",
        state.last_gpu[0] ? ", 显卡 " : "",
        state.last_gpu[0] ? state.last_gpu : "",
        state.auto_disabled ? "已暂停自动尝试 Vulkan, 本次直接用 OpenGL" :
            "本次仍会尝试 Vulkan (未达到暂停阈值)");
}

/* 写盘失败时的硬保险, 与 1.17.6 之前的行为一致: 把用户配置直接写死 OpenGL。
   只有在状态文件写不进去时才走到这里 —— 这条兜底本身不需要用户看见, 但必须留日志,
   否则"为什么设置页里的后端被改了"没有任何线索。 */
static void hard_fallback_to_opengl(BongoCatApp *app) {
    if (app->settings.model.renderer == BONGO_CAT_RENDERER_OPENGL) return;
    app->settings.model.renderer = BONGO_CAT_RENDERER_OPENGL;
    app->dirty = true;
    bongo_cat_config_store_flush(app);
    SDL_LogError(BONGO_CAT_LOG_LIFECYCLE,
        "[render] 自动回退状态无法落盘, 退回旧保险: 把 settings 的 renderer 写死 "
        "opengl, 保证下次启动不会再卡在 Vulkan 上");
}

bool bongo_cat_renderer_fallback_note_failure(BongoCatApp *app,
    const char *reason, const char *detail, const char *gpu) {
    if (!app) return false;
    BongoCatRendererFallbackState *state = &app->renderer_fallback;
    state->consecutive_failures++;
    if (state->consecutive_failures >= BONGO_CAT_RENDERER_FALLBACK_PAUSE_THRESHOLD)
        state->auto_disabled = true;
    snprintf(state->last_reason, sizeof(state->last_reason), "%s",
        reason && reason[0] ? reason : "unknown");
    snprintf(state->last_detail, sizeof(state->last_detail), "%s",
        detail ? detail : "");
    snprintf(state->last_gpu, sizeof(state->last_gpu), "%s", gpu ? gpu : "");
    bongo_cat_runtime_timestamp(state->last_failure_at,
        sizeof(state->last_failure_at));
    const bool saved = state_save(app, state);
    SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
        "[render] 自动回退记录: 连续第 %d 次 (原因=%s, 阈值=%d, 暂停自动尝试=%d)",
        state->consecutive_failures, state->last_reason,
        BONGO_CAT_RENDERER_FALLBACK_PAUSE_THRESHOLD,
        state->auto_disabled ? 1 : 0);
    if (state->auto_disabled)
        SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
            "[render] 连续 %d 次自动回退, 已暂停自动尝试 Vulkan: 以后启动直接用 "
            "OpenGL; 在 设置→性能→渲染后端 点「再试一次 Vulkan」可手动重置",
            state->consecutive_failures);
    else
        SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
            "[render] 已连续 %d 次自动回退 (阈值 %d): 下次启动仍会再尝试一次 Vulkan; "
            "再失败一次就暂停自动尝试",
            state->consecutive_failures,
            BONGO_CAT_RENDERER_FALLBACK_PAUSE_THRESHOLD);
    if (!saved) hard_fallback_to_opengl(app);
    return saved;
}

void bongo_cat_renderer_fallback_note_manual_retry(BongoCatApp *app,
    const char *source) {
    if (!app) return;
    const int failures = app->renderer_fallback.consecutive_failures;
    const bool paused = app->renderer_fallback.auto_disabled;
    if (failures <= 0 && !paused) return;
    state_zero(&app->renderer_fallback);
    const bool saved = state_save(app, &app->renderer_fallback);
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[render] %s手动选择 Vulkan: 已清空自动回退记录 (之前连续 %d 次, 暂停=%d), "
        "%s",
        source && source[0] ? source : "",
        failures, paused ? 1 : 0,
        saved ? "状态已落盘" : "状态落盘失败, 仅本次运行生效");
}

void bongo_cat_renderer_fallback_note_success(BongoCatApp *app) {
    if (!app) return;
    const int failures = app->renderer_fallback.consecutive_failures;
    const bool paused = app->renderer_fallback.auto_disabled;
    if (failures <= 0 && !paused) return;
    state_zero(&app->renderer_fallback);
    const bool saved = state_save(app, &app->renderer_fallback);
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[render] Vulkan 首帧已呈现: 自动回退计数归零 (之前连续 %d 次, 暂停=%d), %s",
        failures, paused ? 1 : 0,
        saved ? "状态已落盘" : "状态落盘失败");
}

bool bongo_cat_renderer_fallback_paused(const BongoCatApp *app) {
    return app && app->renderer_fallback.auto_disabled &&
        app->renderer_fallback.consecutive_failures >=
            BONGO_CAT_RENDERER_FALLBACK_PAUSE_THRESHOLD;
}

bool bongo_cat_renderer_fallback_recorded(const BongoCatApp *app) {
    return app && app->renderer_fallback.consecutive_failures > 0;
}

const char *bongo_cat_renderer_fallback_reason_label(const char *reason) {
    if (!reason || !reason[0]) return "原因未记录";
    if (strcmp(reason, BONGO_CAT_RENDERER_FALLBACK_REASON_TIMEOUT) == 0)
        return "首帧超时 (8 秒内没有画面)";
    if (strcmp(reason, BONGO_CAT_RENDERER_FALLBACK_REASON_INIT) == 0)
        return "Vulkan 初始化失败";
    return reason;
}
