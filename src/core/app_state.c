#include "bongo_cat/app.h"
#include "bongo_cat/overlay.h"

#include <math.h>
#include <string.h>

/*
 * 输入 -> 模型参数的映射层: 把平台层已经归一化过的 BongoCatInputEvent(键盘 /
 * 鼠标 / 手柄)翻译成 Live2D 参数和 overlay 动作。
 *
 * active_inputs 是"当前仍被按住"的输入快照, 存在的唯一理由是模型切换或重新
 * 加载后需要把还按着的键重新施加一遍(见 bongo_cat_app_reapply_input): 新模型
 * 完全不知道之前发生过什么。数组保持紧凑, 顺序即按下顺序。
 */
static size_t active_input_index(const BongoCatApp *app,
    BongoCatInputKind kind, const char *name) {
    if (!app || !name) return app ? app->active_input_count : 0;
    for (size_t i = 0; i < app->active_input_count; ++i) {
        const BongoCatInputEvent *active = &app->active_inputs[i];
        if (active->kind == kind && strcmp(active->name, name) == 0) return i;
    }
    return app->active_input_count;
}

static void active_input_remove(BongoCatApp *app, size_t index) {
    if (!app || index >= app->active_input_count) return;
    if (index + 1 < app->active_input_count)
        memmove(&app->active_inputs[index], &app->active_inputs[index + 1],
            (app->active_input_count - index - 1) * sizeof(app->active_inputs[0]));
    memset(&app->active_inputs[--app->active_input_count], 0,
        sizeof(app->active_inputs[0]));
}

/* 只有"可按住"的事件才进快照(按下/抬起、手柄按钮、摇杆轴), 鼠标点击这种
   瞬时事件不进。抬起和"值回到 0"都走删除分支, 所以先算出 active 再分发。 */
static void active_input_update(BongoCatApp *app,
    const BongoCatInputEvent *event) {
    if (!app || !event || !event->name[0]) return;
    bool tracked = event->kind == BONGO_CAT_INPUT_KEY_DOWN ||
        event->kind == BONGO_CAT_INPUT_KEY_UP ||
        event->kind == BONGO_CAT_INPUT_GAMEPAD_BUTTON ||
        event->kind == BONGO_CAT_INPUT_GAMEPAD_AXIS;
    if (!tracked) return;
    bool active = event->kind == BONGO_CAT_INPUT_KEY_DOWN ||
        (event->kind == BONGO_CAT_INPUT_GAMEPAD_BUTTON && event->value > 0.5f) ||
        (event->kind == BONGO_CAT_INPUT_GAMEPAD_AXIS && fabsf(event->value) > .001f);
    BongoCatInputKind kind = event->kind == BONGO_CAT_INPUT_KEY_DOWN ||
        event->kind == BONGO_CAT_INPUT_KEY_UP
        ? BONGO_CAT_INPUT_KEY_DOWN : event->kind;
    size_t index = active_input_index(app, kind, event->name);
    if (!active) { active_input_remove(app, index); return; }
    if (index == app->active_input_count) {
        if (app->active_input_count >=
            sizeof(app->active_inputs) / sizeof(app->active_inputs[0])) return;
        index = app->active_input_count++;
    }
    app->active_inputs[index] = *event;
    app->active_inputs[index].kind = kind;
}

static bool stick_active(float x, float y, bool pressed) {
    return pressed || fabsf(x) > 0.001f || fabsf(y) > 0.001f;
}

/* 左右手是否放下由两个来源共同决定: 手柄摇杆/摇杆按键, 以及 overlay(模型自带
   的触摸区域)的动作。任意一个为真就要放下, 所以每次改动后都要重算一次。 */
static void update_hands(BongoCatApp *app) {
    bool left_stick = stick_active(app->left_stick_x,
        app->left_stick_y, app->left_stick_pressed);
    bool right_stick = stick_active(app->right_stick_x,
        app->right_stick_y, app->right_stick_pressed);
    bongo_cat_live2d_set_parameter(app->live2d, "CatParamStickShowLeftHand", left_stick);
    bongo_cat_live2d_set_parameter(app->live2d, "CatParamStickShowRightHand", right_stick);
    bongo_cat_live2d_set_parameter(app->live2d, "CatParamLeftHandDown",
        (left_stick || bongo_cat_overlay_hand_active(app->overlay, false)) ? 1.0f : 0.0f);
    bongo_cat_live2d_set_parameter(app->live2d, "CatParamRightHandDown",
        (right_stick || bongo_cat_overlay_hand_active(app->overlay, true)) ? 1.0f : 0.0f);
}

static bool apply_key(BongoCatApp *app, const char *name, bool pressed) {
    int hand = bongo_cat_overlay_key(app->overlay, name, pressed);
    if (hand < 0) return false;
    update_hands(app);
    app->dirty = true;
    return true;
}

/* 摇杆输入取值是 [-1, 1], 而每个参数有自己的区间(还可能不对称), 所以先按
   range.maximum 缩放再钳制。参数不存在就静默跳过 —— 模型允许只实现一部分。 */
static void set_axis(BongoCatApp *app, const char *id, float input) {
    BongoCatParameterRange range;
    if (!bongo_cat_live2d_parameter(app->live2d, id, &range)) return;
    float value = input * range.maximum;
    if (value < range.minimum) value = range.minimum;
    if (value > range.maximum) value = range.maximum;
    bongo_cat_live2d_set_parameter(app->live2d, id, value);
}

/* 手柄事件分三类: 四个摇杆轴(记录最新值并写参数)、两个摇杆按键(记录按下并
   同步 Live2D)、其余按钮/扳机(交给 overlay 的按键映射)。事件名是 SDL 的手柄
   按键名, 属于外部契约, 不能改。 */
static void apply_gamepad(BongoCatApp *app, const BongoCatInputEvent *event) {
    const char *id = NULL;
    if (strcmp(event->name, "LeftStickX") == 0) {
        id = "CatParamStickLX"; app->left_stick_x = event->value;
    } else if (strcmp(event->name, "LeftStickY") == 0) {
        id = "CatParamStickLY"; app->left_stick_y = event->value;
    } else if (strcmp(event->name, "RightStickX") == 0) {
        id = "CatParamStickRX"; app->right_stick_x = event->value;
    } else if (strcmp(event->name, "RightStickY") == 0) {
        id = "CatParamStickRY"; app->right_stick_y = event->value;
    } else if (strcmp(event->name, "LeftThumb") == 0) {
        app->left_stick_pressed = event->value > 0.0f;
        apply_key(app, event->name, app->left_stick_pressed);
        bongo_cat_live2d_set_parameter(app->live2d, "CatParamStickLeftDown",
            app->left_stick_pressed);
    } else if (strcmp(event->name, "RightThumb") == 0) {
        app->right_stick_pressed = event->value > 0.0f;
        apply_key(app, event->name, app->right_stick_pressed);
        bongo_cat_live2d_set_parameter(app->live2d, "CatParamStickRightDown",
            app->right_stick_pressed);
    } else apply_key(app, event->name, event->value > 0.05f);
    if (id) set_axis(app, id, event->value);
    update_hands(app);
    app->dirty = true;
}

/*
 * 手柄断开或失焦时的兜底清理: 系统不会补发"抬起", 所以只能假设所有手柄输入
 * 都已松开, 否则猫会一直举着手。
 * 清理范围是刻意的: 只删掉名字带 "Gamepad:" 前缀的按住记录与高亮标记, 键盘的
 * 部分要留着 —— 用户可能正按着键盘, 只是手柄恰好断线了。
 */
void bongo_cat_app_reset_gamepad(BongoCatApp *app) {
    if (!app || !app->live2d) return;
    for (size_t i = app->sound_shortcut_state.count; i > 0; --i) {
        if (strncmp(app->sound_shortcut_state.held[i - 1], "Gamepad:", 8)) continue;
        --app->sound_shortcut_state.count;
        if (i - 1 != app->sound_shortcut_state.count)
            memcpy(app->sound_shortcut_state.held[i - 1],
                app->sound_shortcut_state.held[app->sound_shortcut_state.count], BONGO_CAT_ID_CAP);
    }
    for (size_t i = 0; i < app->settings.behavior_shortcut_count; ++i)
        if (strstr(app->settings.behavior_shortcuts[i].shortcut, "Gamepad:"))
            app->sound_shortcut_active[i] = false;
    app->left_stick_x = app->left_stick_y = 0.0f;
    app->right_stick_x = app->right_stick_y = 0.0f;
    app->left_stick_pressed = app->right_stick_pressed = false;
    const char *parameters[] = {"CatParamStickLX", "CatParamStickLY",
        "CatParamStickRX", "CatParamStickRY", "CatParamStickLeftDown",
        "CatParamStickRightDown"};
    for (size_t i = 0; i < sizeof(parameters) / sizeof(parameters[0]); ++i)
        bongo_cat_live2d_set_parameter(app->live2d, parameters[i], 0.0f);
    const char *buttons[] = {"South", "East", "West", "North", "Select", "Mode",
        "Start", "LeftTrigger", "RightTrigger", "LeftTrigger2", "RightTrigger2",
        "DPadUp", "DPadDown", "DPadLeft", "DPadRight", "Misc1", "Misc2",
        "Misc3", "Misc4", "Misc5", "Misc6", "RightPaddle1", "LeftPaddle1",
        "RightPaddle2", "LeftPaddle2", "Touchpad", "LeftThumb", "RightThumb"};
    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i)
        apply_key(app, buttons[i], false);
    for (size_t i = app->active_input_count; i > 0; --i)
        if (app->active_inputs[i - 1].kind == BONGO_CAT_INPUT_GAMEPAD_BUTTON ||
            app->active_inputs[i - 1].kind == BONGO_CAT_INPUT_GAMEPAD_AXIS)
            active_input_remove(app, i - 1);
    update_hands(app);
    app->dirty = true;
}

/*
 * 输入总入口, 由平台层的输入链路送进来。不变量: 同一时刻只有一个线程调用它
 * (内部不加锁)。没有模型时仍然统计诊断计数, 以便区分"按键没反应"到底是没加载
 * 模型, 还是按键没映射上。
 */
void bongo_cat_app_apply_input(BongoCatApp *app, const BongoCatInputEvent *event) {
    if (!app || !event) return;
    bool keyboard = event->kind == BONGO_CAT_INPUT_KEY_DOWN ||
        event->kind == BONGO_CAT_INPUT_KEY_UP;
    if (!app->live2d) {
        if (keyboard) app->input_diagnostics.no_model_keys++;
        return;
    }
    active_input_update(app, event);
    switch (event->kind) {
    case BONGO_CAT_INPUT_KEY_DOWN:
    case BONGO_CAT_INPUT_KEY_UP:
        if (apply_key(app, event->name, event->kind == BONGO_CAT_INPUT_KEY_DOWN))
            app->input_diagnostics.mapped_keys++;
        else app->input_diagnostics.unmapped_keys++;
        break;
    /* 左右键分别对应 ParamMouseLeftDown / ParamMouseRightDown; 侧键(Back /
       Forward)没有独立参数, 只把"侧键状态变了"置脏交给上层处理。 */
    case BONGO_CAT_INPUT_MOUSE_DOWN:
    case BONGO_CAT_INPUT_MOUSE_UP: {
        bool left = strcmp(event->name, "Left") == 0;
        bool right = strcmp(event->name, "Right") == 0;
        bool side = strcmp(event->name, "Back") == 0 ||
            strcmp(event->name, "Forward") == 0;
        if (!left && !right && !side)
            break;
        bool down = event->kind == BONGO_CAT_INPUT_MOUSE_DOWN;
        bool changed;
        if (left) {
            changed = app->left_mouse_down != down;
            app->left_mouse_down = down;
        } else if (right) {
            changed = app->right_mouse_down != down;
            app->right_mouse_down = down;
        } else {
            if (strcmp(event->name, "Back") == 0) app->back_mouse_down = down;
            else app->forward_mouse_down = down;
            bool side_down = app->back_mouse_down || app->forward_mouse_down;
            changed = app->side_mouse_down != side_down;
            app->side_mouse_down = side_down;
        }
        if (changed) app->mouse_button_event_pending = true;
        if (!down) app->pointer_hit_dirty = true;
        if (side) {
            app->dirty = true;
            break;
        }
        const char *id = left
            ? "ParamMouseLeftDown" : "ParamMouseRightDown";
        bongo_cat_live2d_set_parameter(app->live2d, id, down ? 1.0f : 0.0f);
        app->dirty = true;
        break;
    }
    case BONGO_CAT_INPUT_GAMEPAD_BUTTON:
    case BONGO_CAT_INPUT_GAMEPAD_AXIS: apply_gamepad(app, event); break;
    default: break;
    }
}

/* 模型(重新)加载后调用: 把鼠标左右键状态和 active_inputs 里仍按着的键重新
   施加一遍, 否则切模型时按住不放的键会"掉", 新模型收不到那次按下事件。 */
void bongo_cat_app_reapply_input(BongoCatApp *app) {
    if (!app || !app->live2d) return;
    bongo_cat_live2d_set_parameter(app->live2d, "ParamMouseLeftDown",
        app->left_mouse_down ? 1.0f : 0.0f);
    bongo_cat_live2d_set_parameter(app->live2d, "ParamMouseRightDown",
        app->right_mouse_down ? 1.0f : 0.0f);
    for (size_t i = 0; i < app->active_input_count; ++i) {
        BongoCatInputEvent *event = &app->active_inputs[i];
        if (event->kind == BONGO_CAT_INPUT_KEY_DOWN)
            apply_key(app, event->name, true);
        else if (event->kind == BONGO_CAT_INPUT_GAMEPAD_BUTTON ||
            event->kind == BONGO_CAT_INPUT_GAMEPAD_AXIS)
            apply_gamepad(app, event);
    }
    update_hands(app);
    app->dirty = true;
}
