/*
 * --ci-runtime-flow 的脚本化流程: 每 1.5s 推进一步, 每步做一件"用户常常做、却很难
 * 自动化"的事 (缩放 / 透明度 / 切模型 / 打开设置选模型 / 恢复), 并把步骤名写进
 * runtime-flow-stage.txt —— CI 靠这个文件判断流程卡在哪一步。
 * 没有 --ci-runtime-flow 时第一行就返回, 生产运行完全不走这里。
 */
#include "runtime.h"
#include "bongo_cat/file.h"
#include "bongo_cat/path.h"
#include "bongo_cat/preferences.h"

#include <stdio.h>
#include <string.h>

/* 步骤标记纯给外部 (CI) 看, 不做任何同步; 写失败也不影响流程。 */
static void mark(BongoCatApp *app, const char *name) {
    char path[BONGO_CAT_PATH_CAP];
    bongo_cat_path_join(path, sizeof(path), app->state_root, "runtime-flow-stage.txt");
    FILE *file = bongo_cat_file_open(path, "wb");
    if (file) { fputs(name, file); fclose(file); }
}

static void scale(BongoCatApp *app, float value) {
    bongo_cat_window_cancel_wheel_animation(app);
    bongo_cat_window_set_scale(app, value);
}

static void opacity(BongoCatApp *app, float value) {
    app->session.window.opacity_percent = value;
    bongo_cat_app_cancel_hover_fade(app);
    bongo_cat_platform_set_opacity(&app->platform, value / 100.0f);
}

/*
 * 换模型一律走异步队列 (见 model_switch.c): 同步加载会让主循环卡住好几秒, 而这条流程存在的
 * 意义恰恰是"无人值守地跑用户常做的事", 卡住会让 CI 里量到的帧序列失去意义。
 * 请求返回假 (模型不存在) 时只记一行日志 —— 流程脚本没拿到结果也不该中断整轮自测。
 */
static void model(BongoCatApp *app, size_t index) {
    if (index >= app->models.count) return;
    BongoCatError error = {0};
    if (bongo_cat_app_request_model_switch(app, app->models.entries[index].id,
            &error)) return;
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
        "Runtime flow could not request model %s: %s",
        app->models.entries[index].id,
        error.message[0] ? error.message : "unknown error");
}

static void model_mode(BongoCatApp *app, BongoCatModelMode mode) {
    for (size_t i = 0; i < app->models.count; ++i)
        if (app->models.entries[i].mode == mode) { model(app, i); return; }
}

/* 回到这轮流程开始时记下的模型 (smoke_runtime_model)。同样走异步队列。 */
static void restore_initial_model(BongoCatApp *app) {
    if (!app->smoke_runtime_model[0]) return;
    BongoCatError error = {0};
    if (bongo_cat_app_request_model_switch(app, app->smoke_runtime_model,
            &error)) return;
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
        "Runtime flow could not restore model %s: %s", app->smoke_runtime_model,
        error.message[0] ? error.message : "unknown error");
}

/* target 只增不减 (由 elapsed 算出来), 所以掉帧/卡顿不会重复执行同一步, 每步只跑一次;
   超过最后一步就直接结束进程。 */
void bongo_cat_runtime_flow_update(BongoCatApp *app, uint64_t now) {
    if (!app || !app->smoke_runtime_flow) return;
    if (!app->smoke_runtime_flow_ns) {
        app->smoke_runtime_flow_ns = now;
        snprintf(app->smoke_runtime_model, sizeof(app->smoke_runtime_model), "%.*s",
            (int)sizeof(app->smoke_runtime_model) - 1, app->session.active_model_id);
        mark(app, "startup"); return;
    }
    uint64_t elapsed = now - app->smoke_runtime_flow_ns;
    unsigned target = (unsigned)(elapsed / 1500000000ull);
    if (target <= app->smoke_runtime_stage) return;
    app->smoke_runtime_stage = target;
    switch (target) {
    case 1: mark(app, "idle"); break;
    case 2: scale(app, 125.0f); mark(app, "scale-up"); break;
    case 3: scale(app, 75.0f); mark(app, "scale-down"); break;
    case 4: opacity(app, 50.0f); mark(app, "opacity-50"); break;
    case 5: opacity(app, 100.0f); mark(app, "opacity-100"); break;
    case 6: model_mode(app, BONGO_CAT_MODE_KEYBOARD); mark(app, "model-keyboard"); break;
    case 7: model_mode(app, BONGO_CAT_MODE_STANDARD); mark(app, "model-standard"); break;
    case 8: model_mode(app, BONGO_CAT_MODE_GAMEPAD); mark(app, "model-gamepad"); break;
/* 设置窗口这几步验证的是"打开设置 -> 选模型 -> 关闭 -> 模型是否恢复"; 步骤 11/13/15
   都带恢复动作, 让每一步的断言不受前一步残留状态影响。 */
    case 9:
        app->smoke_preference_model_select = true;
        bongo_cat_preferences_show(app->preferences);
        mark(app, "settings-first-model-select"); break;
    case 10: mark(app, "settings-idle"); break;
    case 11:
        bongo_cat_preferences_close(app->preferences);
        model_mode(app, BONGO_CAT_MODE_STANDARD);
        mark(app, "settings-closed-model-restored"); break;
    case 12:
        app->smoke_preference_model_select = true;
        bongo_cat_preferences_show(app->preferences);
        mark(app, "settings-reopen-model-select"); break;
    case 13:
        bongo_cat_preferences_close(app->preferences);
        restore_initial_model(app);
        mark(app, "recovery"); break;
    case 14: model_mode(app, BONGO_CAT_MODE_GAMEPAD); mark(app, "gamepad-repeat"); break;
    case 15:
        restore_initial_model(app);
        mark(app, "final-recovery"); break;
    default: app->running = false; break;
    }
}
