/*
 * 手柄输入入口: 把 SDL 手柄事件翻译成 BongoCatInputEvent, 并维护"哪一只手柄是活动设备"。
 * SDL 只允许在事件回调里读手柄对象, 所以这里也负责开/关句柄 —— app->active_gamepad 是
 * 唯一的活动设备 ID (0 = 没有), 非活动设备的句柄必须马上关掉, 否则拔插几次就会泄漏。
 * 事件名直接沿用 SDL 的按键/轴名字 (South / LeftStickX / LeftTrigger2 ...), 它是外部契约:
 * core/app_state.c 按字符串匹配成模型参数, 覆盖层的按键图也按这些名字查找, 不能改名。
 * 轴值在这里归一化成 [-1,1] (负数除以 32768, 正数除以 32767), 到 core 再乘参数区间上限。
 * 断开连接时系统不会补发"抬起", 所以必须先 bongo_cat_app_reset_gamepad 清状态。
 */
#include "runtime.h"
#include "bongo_cat/log.h"

#include <stdio.h>
#include <string.h>

const char *bongo_cat_gamepad_axis_name(Uint8 axis) {
    switch (axis) {
    case SDL_GAMEPAD_AXIS_LEFTX: return "LeftStickX";
    case SDL_GAMEPAD_AXIS_LEFTY: return "LeftStickY";
    case SDL_GAMEPAD_AXIS_RIGHTX: return "RightStickX";
    case SDL_GAMEPAD_AXIS_RIGHTY: return "RightStickY";
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return "LeftTrigger2";
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return "RightTrigger2";
    default: return "UnknownAxis";
    }
}

const char *bongo_cat_gamepad_button_name(Uint8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return "South";
    case SDL_GAMEPAD_BUTTON_EAST: return "East";
    case SDL_GAMEPAD_BUTTON_WEST: return "West";
    case SDL_GAMEPAD_BUTTON_NORTH: return "North";
    case SDL_GAMEPAD_BUTTON_BACK: return "Select";
    case SDL_GAMEPAD_BUTTON_GUIDE: return "Mode";
    case SDL_GAMEPAD_BUTTON_START: return "Start";
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "LeftThumb";
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "RightThumb";
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "LeftTrigger";
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "RightTrigger";
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return "DPadUp";
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "DPadDown";
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "DPadLeft";
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "DPadRight";
    case SDL_GAMEPAD_BUTTON_MISC1: return "Misc1";
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1: return "RightPaddle1";
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1: return "LeftPaddle1";
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2: return "RightPaddle2";
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2: return "LeftPaddle2";
    case SDL_GAMEPAD_BUTTON_TOUCHPAD: return "Touchpad";
    case SDL_GAMEPAD_BUTTON_MISC2: return "Misc2";
    case SDL_GAMEPAD_BUTTON_MISC3: return "Misc3";
    case SDL_GAMEPAD_BUTTON_MISC4: return "Misc4";
    case SDL_GAMEPAD_BUTTON_MISC5: return "Misc5";
    case SDL_GAMEPAD_BUTTON_MISC6: return "Misc6";
    default: return "UnknownButton";
    }
}

/* 只关掉非活动设备的句柄: 活动设备由 active_gamepad 继续持有, 关了就没法收事件了。 */
static void close_inactive_gamepads(BongoCatApp *app, const SDL_JoystickID *ids,
    int count) {
    for (int i = 0; ids && i < count; ++i) {
        SDL_Gamepad *gamepad = SDL_GetGamepadFromID(ids[i]);
        if (gamepad && ids[i] != app->active_gamepad) SDL_CloseGamepad(gamepad);
    }
}

/* 没有活动设备时的选择策略: 取第一台能打开的设备; 一台都没有就保持 0 并清干净旧状态。
   返回是否选到了设备 (ids 由 SDL_GetGamepads 分配, 本函数负责 SDL_free)。 */
static bool select_first_gamepad(BongoCatApp *app) {
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    SDL_JoystickID selected = 0;
    for (int i = 0; ids && i < count && !selected; ++i) {
        SDL_Gamepad *gamepad = SDL_GetGamepadFromID(ids[i]);
        if (!gamepad) gamepad = SDL_OpenGamepad(ids[i]);
        if (gamepad) selected = ids[i];
    }
    app->active_gamepad = selected;
    close_inactive_gamepads(app, ids, count);
    SDL_free(ids);
    return selected != 0;
}

/*
 * 预热手柄子系统 (幂等, 由启动阶段调用一次)。
 *
 * 为什么要在启动时做: SDL_InitSubSystem(SDL_INIT_GAMEPAD) 在 Windows 上要枚举 HID 设备,
 * 实测第一次超过 600ms。它过去是"第一次切到手柄模型"时才发生 —— 也就是挂在那一次切换的
 * 收尾里, 表现成"换模型换到最后一刻画面冻住 600ms"。启动时窗口还没显示, 这 600ms 用户
 * 看不到。
 *
 * 为什么不改变输入行为: 这里只是让子系统就绪, 真正把手柄事件喂给模型仍然要求
 * loaded_mode == BONGO_CAT_MODE_GAMEPAD (见 bongo_cat_gamepad_event 与 select_model_state)。
 * 子系统在"切回键鼠模型"时会被 set_enabled(false) 关掉, 于是下一次切到手柄模型仍要付这
 * 一次初始化 —— 那是既有行为, 不在本次修复范围内。
 *
 * 注意 SDL_INIT_GAMEPAD 这个 token 只允许出现在本文件 (见
 * cmake/CheckPlatformRuntimeSafety.cmake 的"所有权"规则), 所以别的模块需要预热时必须调
 * 这个函数, 而不是自己写 SDL_InitSubSystem。
 */
void bongo_cat_gamepads_prewarm(BongoCatApp *app) {
    if (!app) return;
    if (SDL_WasInit(SDL_INIT_GAMEPAD) & SDL_INIT_GAMEPAD) return;
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
            "Gamepad prewarm failed (keyboard and mouse input still work): %s",
            SDL_GetError());
        return;
    }
    SDL_LogInfo(BONGO_CAT_LOG_INPUT,
        "[input] 手柄子系统已预热 (首次初始化约数百毫秒, 放在启动阶段执行)");
}

/* 手柄开关的统一入口 (切到手柄模式 / 切回键鼠模式 / 退出时关闭)。enabled 为真时初始化
   手柄子系统并保证有一台活动设备; 为假时关闭全部句柄、清掉"按住"状态并退出子系统。
    可重复调用: 子系统已经初始化过时不会再初始化一次。 */
void bongo_cat_gamepads_set_enabled(BongoCatApp *app, bool enabled) {
    if (!app) return;
    bool initialized = (SDL_WasInit(SDL_INIT_GAMEPAD) & SDL_INIT_GAMEPAD) != 0;
    if (enabled && !initialized) {
        /* 桌宠通常不是前台窗口, 不开这个提示就拿不到手柄输入。 */
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                "Gamepad initialization failed: %s", SDL_GetError());
            /* 初始化失败要降级成纯键鼠: 清掉 active_gamepad 与残留的"按住"状态, 否则模型
               会停在按下姿势, 而系统再也不会有事件来解除它。 */
            app->active_gamepad = 0;
            bongo_cat_app_reset_gamepad(app);
            return;
        }
        initialized = true;
    }
    int count = 0;
    SDL_JoystickID *ids = initialized ? SDL_GetGamepads(&count) : NULL;
    bool active_connected = false;
    for (int i = 0; ids && i < count; ++i) {
        if (ids[i] == app->active_gamepad && SDL_GetGamepadFromID(ids[i]))
            active_connected = true;
        if (!enabled) {
            SDL_Gamepad *gamepad = SDL_GetGamepadFromID(ids[i]);
            if (gamepad) SDL_CloseGamepad(gamepad);
        }
    }
    SDL_free(ids);
    /* 关闭路径: 先断开活动设备再退出子系统。顺序反过来的话 active_gamepad 会变成指向
       已释放设备的 ID。 */
    if (!enabled) {
        app->active_gamepad = 0;
        bongo_cat_app_reset_gamepad(app);
        if (initialized) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    } else if (active_connected) {
        ids = SDL_GetGamepads(&count);
        close_inactive_gamepads(app, ids, count);
        SDL_free(ids);
    } else {
        app->active_gamepad = 0;
        bongo_cat_app_reset_gamepad(app);
        select_first_gamepad(app);
    }
}

/* SDL 手柄事件入口。raw 实际类型是 const SDL_Event*, 用 void* 是为了让 runtime.h 不必
   拖进 SDL 头文件; 调用方 (app_loop 的事件分发) 保证它一定是 SDL_Event。
   只接受活动设备的事件: 副手柄与手柄键鼠模拟都被丢弃。 */
void bongo_cat_gamepad_event(BongoCatApp *app, const void *raw) {
    const SDL_Event *event = raw;
    if (event->type == SDL_EVENT_GAMEPAD_ADDED) {
        if (app->loaded_mode == BONGO_CAT_MODE_GAMEPAD &&
            !app->active_gamepad) {
            SDL_Gamepad *gamepad = SDL_GetGamepadFromID(event->gdevice.which);
            if (!gamepad) gamepad = SDL_OpenGamepad(event->gdevice.which);
            if (gamepad) app->active_gamepad = event->gdevice.which;
        }
        return;
    }
    if (event->type == SDL_EVENT_GAMEPAD_REMOVED) {
        SDL_Gamepad *gamepad = SDL_GetGamepadFromID(event->gdevice.which);
        if (gamepad) SDL_CloseGamepad(gamepad);
        if (event->gdevice.which == app->active_gamepad) {
            app->active_gamepad = 0;
            bongo_cat_app_reset_gamepad(app);
            if (app->loaded_mode == BONGO_CAT_MODE_GAMEPAD)
                select_first_gamepad(app);
        }
        return;
    }
    if (app->loaded_mode != BONGO_CAT_MODE_GAMEPAD) return;
    BongoCatInputEvent input = {0};
    SDL_JoystickID source;
    if (event->type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
        source = event->gaxis.which;
        input.kind = BONGO_CAT_INPUT_GAMEPAD_AXIS;
        /* 负数除以 32768、正数除以 32767: 这样 -32768 与 +32767 都能落到 ±1.0。 */
        input.value = event->gaxis.value < 0
            ? event->gaxis.value / 32768.0f : event->gaxis.value / 32767.0f;
        snprintf(input.name, sizeof(input.name), "%s", bongo_cat_gamepad_axis_name(event->gaxis.axis));
    /* 按键是 0/1 脉冲、轴是连续量: core 的"按住状态"快照靠这个区分"值回到 0"的轴事件。 */
    } else if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
        event->type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
        source = event->gbutton.which;
        input.kind = BONGO_CAT_INPUT_GAMEPAD_BUTTON;
        input.value = event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? 1.0f : 0.0f;
        snprintf(input.name, sizeof(input.name), "%s", bongo_cat_gamepad_button_name(event->gbutton.button));
    } else return;
    /* SDL 会给所有已打开的设备送事件, 非活动设备的事件在这里丢掉。 */
    if (!app->active_gamepad || source != app->active_gamepad) return;
    /* 未映射的按键/轴返回 "Unknown*": 放过去会让参数与覆盖层拿到不存在的名字, 直接丢弃。 */
    if (strncmp(input.name, "Unknown", 7) == 0) return;
    input.timestamp_ms = SDL_GetTicks();
    /* 先过快捷键: 命中后仍要把事件交给 apply_input 推进按键状态, 否则抬起被吞掉会让模型
       卡在按下姿势。 */

    if (input.kind == BONGO_CAT_INPUT_GAMEPAD_BUTTON)
        bongo_cat_app_shortcuts(app, &input);
    bongo_cat_app_apply_input(app, &input);
}
