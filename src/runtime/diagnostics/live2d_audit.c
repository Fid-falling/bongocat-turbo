/* Live2D 冒烟场景的总入口: 场景名来自命令行 --ci-live2d-scenario= (见 startup_arguments.c),
 * 存在 app->smoke_live2d_scenario 里。apply() 负责"施加操作", assertions() 负责"读回参数判定",
 * 两者刻意分开: 断言失败只影响 live2d-audit.txt 里的 assertions= 字段, 只有 cubism 原生构建才会
 * 把失败升级成 app->exit_code=1, 因为 stub 构建没有真实模型可判。断言读到的参数值是"上一帧提交后"
 * 的状态, 所以需要额外步进的场景必须自己推进帧 (见 parameter_override_scenario 之后的 4 帧)。
 */
#include "runtime.h"
#include "live2d_audit_scenario.h"
#include "live2d_pointer_audit.h"
#include "bongo_cat/file.h"
#include "bongo_cat/path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 本文件内的三个缩写别名, 只为了让下面的场景分发读起来像表格; 真正的实现在 live2d_audit_scenario.c
 * 与模型层, 不要在别处复用这些宏。
 */
#define input bongo_cat_live2d_audit_input
#define motion bongo_cat_live2d_audit_motion
#define parameter_override_scenario bongo_cat_live2d_audit_parameter_override

/* 参数不存在时返回 false 且不写 *output ——"模型没有这个参数"和"参数值恰好是 0"必须区分开,
 * 断言里大量依赖这个区别 (例如 inactive() 要求参数存在且接近 0)。
 */
static bool value(BongoCatApp *app, const char *id, float *output) {
    BongoCatParameterRange range;
    if (!bongo_cat_live2d_parameter(app->live2d, id, &range)) return false;
    if (output) *output = range.value;
    return true;
}

/* Viewer 5.3.03: (7.2727275 / 30) target speed * 30-degree range * 1.2 gain. */
#define POINTER_VISIBLE_STEP_LIMIT 8.8f

/* 返回值 = 这个场景的操作是否被接受 (模型能切、动作/表情存在、投影算得出来), 不代表断言通过。
 * "switch:<model-id>" 的后半段就是目标模型 id; 认不出来的名字交给 motion-N / expression-N 处理。
 * 走完 if-else 链的分支末尾统一置 app->dirty 触发重绘, 提前 return 的分支自己负责。
 */
static bool apply(BongoCatApp *app, const char *scenario) {
    if (strncmp(scenario, "switch:", 7) == 0)
        return bongo_cat_app_select_model(app, scenario + 7);
    if (strcmp(scenario, "visual-consistency") == 0)
        return bongo_cat_live2d_visual_audit_run(app);
    if (strcmp(scenario, "viewer-sequence") == 0)
        return bongo_cat_live2d_viewer_audit_run(app);
    if (strcmp(scenario, "mouse-screen") == 0)
        return bongo_cat_app_audit_screen_pointer(app);
    if (strcmp(scenario, "mouse-hand-screen") == 0)
        return bongo_cat_app_audit_display_pointer(app);
    if (strcmp(scenario, "mirror") == 0) app->settings.model.mirror = true;
    else if (strcmp(scenario, "mouse-move") == 0)
        return bongo_cat_live2d_pointer_audit_run(app, false);
    else if (strcmp(scenario, "mouse-move-mirror") == 0)
        return bongo_cat_live2d_pointer_audit_run(app, true);
    else if (strcmp(scenario, "mouse-reverse") == 0)
        return bongo_cat_live2d_pointer_reverse_audit_run(app);
    else if (strcmp(scenario, "key-left") == 0)
        input(app, BONGO_CAT_INPUT_KEY_DOWN, "KeyA", 1.0f);
    else if (strcmp(scenario, "key-tab-left") == 0)
        input(app, BONGO_CAT_INPUT_KEY_DOWN, "Tab", 1.0f);
    else if (strcmp(scenario, "key-right") == 0)
        input(app, BONGO_CAT_INPUT_KEY_DOWN, "RightArrow", 1.0f);
    else if (strcmp(scenario, "key-left-release") == 0) {
        input(app, BONGO_CAT_INPUT_KEY_DOWN, "KeyA", 1.0f);
        input(app, BONGO_CAT_INPUT_KEY_UP, "KeyA", 0.0f);
    } else if (strcmp(scenario, "keys-both-release") == 0) {
        input(app, BONGO_CAT_INPUT_KEY_DOWN, "KeyA", 1.0f);
        input(app, BONGO_CAT_INPUT_KEY_DOWN, "RightArrow", 1.0f);
        input(app, BONGO_CAT_INPUT_KEY_UP, "KeyA", 0.0f);
        input(app, BONGO_CAT_INPUT_KEY_UP, "RightArrow", 0.0f);
    } else if (strcmp(scenario, "key-stress") == 0) {
        for (int i = 0; i < 250; ++i) {
            input(app, BONGO_CAT_INPUT_KEY_DOWN, "KeyA", 1.0f);
            input(app, BONGO_CAT_INPUT_KEY_DOWN, "RightArrow", 1.0f);
            input(app, BONGO_CAT_INPUT_KEY_UP, "KeyA", 0.0f);
            input(app, BONGO_CAT_INPUT_KEY_UP, "RightArrow", 0.0f);
        }
    }
    else if (strcmp(scenario, "keys-both") == 0) {
        input(app, BONGO_CAT_INPUT_KEY_DOWN, "KeyA", 1.0f);
        input(app, BONGO_CAT_INPUT_KEY_DOWN, "RightArrow", 1.0f);
    } else if (strcmp(scenario, "mouse-left") == 0)
        input(app, BONGO_CAT_INPUT_MOUSE_DOWN, "Left", 1.0f);
    else if (strcmp(scenario, "mouse-right") == 0)
        input(app, BONGO_CAT_INPUT_MOUSE_DOWN, "Right", 1.0f);
    else if (strcmp(scenario, "gamepad-buttons") == 0) {
        input(app, BONGO_CAT_INPUT_GAMEPAD_BUTTON, "DPadLeft", 1.0f);
        input(app, BONGO_CAT_INPUT_GAMEPAD_BUTTON, "South", 1.0f);
    } else if (strcmp(scenario, "gamepad-sticks") == 0) {
        input(app, BONGO_CAT_INPUT_GAMEPAD_AXIS, "LeftStickX", .75f);
        input(app, BONGO_CAT_INPUT_GAMEPAD_AXIS, "LeftStickY", -.5f);
        input(app, BONGO_CAT_INPUT_GAMEPAD_AXIS, "RightStickX", -.65f);
        input(app, BONGO_CAT_INPUT_GAMEPAD_AXIS, "RightStickY", .5f);
        input(app, BONGO_CAT_INPUT_GAMEPAD_BUTTON, "LeftThumb", 1.0f);
        input(app, BONGO_CAT_INPUT_GAMEPAD_BUTTON, "RightThumb", 1.0f);
    } else return motion(app, scenario);
    app->dirty = true;
    return true;
}

/* 把参数值写进审计文件: 参数不存在时写 unavailable 而不是 0, 这样 CI 能看出"模型缺这个参数"与
 * "参数真的是 0"的区别; 方括号里是该参数的取值范围, 供人工核对。
 */
static void parameter(FILE *file, BongoCatApp *app, const char *id) {
    BongoCatParameterRange range;
    if (bongo_cat_live2d_parameter(app->live2d, id, &range))
        fprintf(file, "parameter.%s=%.4f [%.4f,%.4f]\n", id,
            range.value, range.minimum, range.maximum);
    else fprintf(file, "parameter.%s=unavailable\n", id);
}

/* 三个判定助手, 阈值集中在这里: >0.5 算激活, <=0.5 算未激活, |值|>0.05 才算有方向。
 * 参数缺失时一律 false —— 把"没这个参数"当成"未激活"会让断言假通过。
 */
static bool active(BongoCatApp *app, const char *id) {
    float current = 0.0f;
    return value(app, id, &current) && current > 0.5f;
}

static bool inactive(BongoCatApp *app, const char *id) {
    float current = 0.0f;
    return value(app, id, &current) && current <= 0.5f;
}

static bool signed_value(BongoCatApp *app, const char *id, bool positive) {
    float current = 0.0f;
    return value(app, id, &current) && (positive ? current > 0.05f : current < -0.05f);
}

/* operation 为假说明操作本身就被拒绝了, 直接判失败、不再读参数 (此时参数值没有意义)。
 * switch: 场景比的是 session.active_model_id (会话目标), 而不是 loaded_model (可能还没加载完)。
 * 返回 false 会让调用方在 cubism 构建下把 exit_code 置 1。
 */
static bool assertions(BongoCatApp *app, const char *scenario, bool operation) {
    if (!operation) return false;
    if (strncmp(scenario, "switch:", 7) == 0)
        return strcmp(app->session.active_model_id, scenario + 7) == 0;
    if (strcmp(scenario, "idle") == 0 || strcmp(scenario, "mouse-screen") == 0 || strcmp(scenario, "mouse-hand-screen") == 0 || strcmp(scenario, "mirror") == 0 ||
        strcmp(scenario, "visual-consistency") == 0 ||
        strcmp(scenario, "viewer-sequence") == 0 ||
        strncmp(scenario, "motion-", 7) == 0) return true;
    if (strncmp(scenario, "expression-", 11) == 0)
        return bongo_cat_live2d_expression(app->live2d) == atoi(scenario + 11);
    if (strcmp(scenario, "key-left") == 0 || strcmp(scenario, "key-tab-left") == 0)
        return active(app, "CatParamLeftHandDown");
    if (strcmp(scenario, "key-right") == 0)
        return active(app, "CatParamRightHandDown");
    if (strcmp(scenario, "keys-both") == 0 ||
        strcmp(scenario, "gamepad-buttons") == 0)
        return active(app, "CatParamLeftHandDown") &&
            active(app, "CatParamRightHandDown");
    if (strcmp(scenario, "key-left-release") == 0)
        return inactive(app, "CatParamLeftHandDown");
    if (strcmp(scenario, "keys-both-release") == 0 ||
        strcmp(scenario, "key-stress") == 0)
        return inactive(app, "CatParamLeftHandDown") &&
            inactive(app, "CatParamRightHandDown");
    if (strcmp(scenario, "mouse-left") == 0)
        return active(app, "ParamMouseLeftDown");
    if (strcmp(scenario, "mouse-right") == 0)
        return active(app, "ParamMouseRightDown");
    if (strcmp(scenario, "mouse-move") == 0) {
        float angle_z = 0.0f;
        bool z_valid = !value(app, "ParamAngleZ", &angle_z) ||
            angle_z < -0.05f;
        return signed_value(app, "ParamAngleX", true) &&
            signed_value(app, "ParamAngleY", true) && z_valid;
    }
    if (strcmp(scenario, "mouse-move-mirror") == 0)
        return signed_value(app, "ParamAngleX", false) &&
            signed_value(app, "ParamAngleY", true);
    if (strcmp(scenario, "mouse-reverse") == 0) {
        bool horizontal =
            bongo_cat_pointer_audit.angle_x[0] < -5.0f &&
            bongo_cat_pointer_audit.angle_x[1] > 5.0f &&
            bongo_cat_pointer_audit.angle_x[2] < -5.0f &&
            bongo_cat_pointer_audit.angle_x[3] > 5.0f;
        bool direction = bongo_cat_pointer_audit.ran && horizontal &&
            bongo_cat_pointer_audit.angle_y[0] > 5.0f &&
            bongo_cat_pointer_audit.angle_y[1] > 5.0f &&
            bongo_cat_pointer_audit.angle_y[2] < -5.0f &&
            bongo_cat_pointer_audit.angle_y[3] < -5.0f;
        return direction &&
            bongo_cat_pointer_audit.maximum_step < POINTER_VISIBLE_STEP_LIMIT;
    }
    if (strcmp(scenario, "gamepad-sticks") == 0)
        return signed_value(app, "CatParamStickLX", true) &&
            signed_value(app, "CatParamStickLY", false) &&
            signed_value(app, "CatParamStickRX", false) &&
            signed_value(app, "CatParamStickRY", true) &&
            active(app, "CatParamLeftHandDown") &&
            active(app, "CatParamRightHandDown");
    return false;
}

/* 入口: 没有 --ci-live2d-scenario= 时立即返回, 非冒烟运行零开销。按键/手柄/鼠标按下这类场景需要
 * 额外步进 4 帧参数才会真正提交到模型; 结果写 state_root/live2d-audit.txt, stub 构建只写
 * renderer=diagnostic 并跳过失败升级。
 */
void bongo_cat_live2d_audit_run(BongoCatApp *app) {
    if (!app || !app->smoke_live2d_scenario[0]) return;
    uint64_t started = SDL_GetTicksNS();
    bool result = apply(app, app->smoke_live2d_scenario);
    if (result && parameter_override_scenario(app->smoke_live2d_scenario))
        for (int frame = 0; frame < 4; ++frame)
            bongo_cat_app_step_live2d(app, 1.0f / 60.0f);
    double duration_ms = (SDL_GetTicksNS() - started) / 1000000.0;
    char path[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(path, sizeof(path), app->state_root,
        "live2d-audit.txt")) return;
    FILE *file = bongo_cat_file_open(path, "wb");
    if (!file) return;
    bool verified = assertions(app, app->smoke_live2d_scenario, result);
    fprintf(file, "scenario=%s\nmodel=%s\nmode=%s\noperation=%s\nassertions=%s\n"
        "duration_ms=%.3f\n",
        app->smoke_live2d_scenario, app->session.active_model_id,
        bongo_cat_mode_name(app->loaded_mode), result ? "accepted" : "rejected",
        verified ? "passed" : "failed", duration_ms);
#ifdef BONGO_CAT_HAS_CUBISM
    fputs("renderer=cubism-native\n", file);
    if (!verified) app->exit_code = 1;
#else
    fputs("renderer=diagnostic; visual-model-result=blocked\n", file);
#endif
    const char *parameters[] = {"CatParamLeftHandDown", "CatParamRightHandDown",
        "CatParamStickLX", "CatParamStickLY", "CatParamStickRX", "CatParamStickRY",
        "CatParamStickLeftDown", "CatParamStickRightDown",
        "CatParamStickShowLeftHand", "CatParamStickShowRightHand",
        "ParamMouseLeftDown", "ParamMouseRightDown", "ParamMouseX", "ParamMouseY",
        "ParamAngleX", "ParamAngleY", "ParamAngleZ", "ParamBodyAngleX",
        "ParamEyeBallX", "ParamEyeBallY"};
    for (size_t i = 0; i < sizeof(parameters) / sizeof(parameters[0]); ++i)
        parameter(file, app, parameters[i]);
    /* 指针四角数据只有 mouse-reverse 场景会填 (ran=true), 其他场景这整段都不输出,
     * 所以读审计文件的人不能假定 pointer.* 一定存在。
     */
    if (bongo_cat_pointer_audit.ran) {
        static const char *names[] = {"tl", "tr", "bl", "br"};
        fprintf(file, "pointer.maximum_step=%.4f\npointer.has_mouse=%d\n",
            bongo_cat_pointer_audit.maximum_step,
            bongo_cat_pointer_audit.has_mouse);
        for (int corner = 0; corner < 4; ++corner)
            fprintf(file, "pointer.%s.angle_x=%.4f\n"
                "pointer.%s.angle_y=%.4f\n"
                "pointer.%s.mouse_x=%.4f\n"
                "pointer.%s.mouse_y=%.4f\n", names[corner],
                bongo_cat_pointer_audit.angle_x[corner], names[corner],
                bongo_cat_pointer_audit.angle_y[corner], names[corner],
                bongo_cat_pointer_audit.mouse_x[corner], names[corner],
                bongo_cat_pointer_audit.mouse_y[corner]);
    }
    fclose(file);
}
