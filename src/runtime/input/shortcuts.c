/*
 * 快捷键分发: 5 条全局快捷键 (显示/隐藏宠物、设置窗口、镜像、点击穿透、窗口置顶) → 行为
 * (动作/表情/音效) 快捷键 → 数字键别名, 三者按固定顺序尝试。
 * 上游是 app_input.c 的 drain_input (模态窗口/录制快捷键时只推进状态机、不调本文件), 下游
 * 是覆盖层与应用状态; 手柄按键也走这里, 但它不参与键盘快捷键状态机。
 * 两条必须遵守的契约: 快捷键状态机每轮都要推进 (动作可以被抑制, 状态推进不能停, 否则松开
 * 的键会永远停在按下状态); 命中判定顺序不能动, 顺序即优先级。
 */
#include "runtime.h"
#include "bongo_cat/overlay.h"
#include "bongo_cat/preferences.h"
#include "bongo_cat/shortcut.h"

#include <stdio.h>
#include <string.h>

/* 宠物可见性开关。放在这里而不是窗口模块: 它要同时处理会话状态、启动首帧与穿透刷新。 */
static void toggle_pet_visibility(BongoCatApp *app) {
    bongo_cat_window_set_visible(app, !app->session.window.visible);
}

/* 判断"这个被隐藏的切换动作"是不是该让位: 同一个开关键上如果还绑了同组且当前可见的动作,
   就交给那个可见的执行, 避免"按一下什么都没发生"。快捷键字符串按存储形式严格比较。 */
static bool hidden_toggle_has_visible_binding(BongoCatApp *app,
    const BongoCatBehaviorEntry *behavior, const char *shortcut) {
    if (!app || !behavior || !shortcut || !shortcut[0] ||
        behavior->kind != BONGO_CAT_BEHAVIOR_MOTION ||
        bongo_cat_live2d_motion_visible(app->live2d,
            behavior->group, behavior->index)) return false;
    for (size_t i = 0; i < app->behaviors.count; ++i) {
        const BongoCatBehaviorEntry *candidate = &app->behaviors.entries[i];
        if (candidate == behavior ||
            candidate->kind != BONGO_CAT_BEHAVIOR_MOTION ||
            !bongo_cat_live2d_motion_visible(app->live2d,
                candidate->group, candidate->index) ||
            !bongo_cat_live2d_motion_same_toggle(app->live2d,
                behavior->group, behavior->index,
                candidate->group, candidate->index)) continue;
        for (size_t j = 0; j < app->settings.behavior_shortcut_count; ++j) {
            const BongoCatBehaviorShortcut *binding =
                &app->settings.behavior_shortcuts[j];
            if (strcmp(binding->id, candidate->id) == 0 &&
                strcmp(binding->shortcut, shortcut) == 0) return true;
        }
    }
    return false;
}

/* 行为快捷键: 逐条绑定比对, 命中就执行。momentary 的行为在抬起时结束 (例如按住说话),
   普通行为在按下时触发; sound_edge 表示这次事件里音效状态机刚变化过。 */
static bool behavior_shortcut(BongoCatApp *app, const BongoCatInputEvent *event,
    bool handled, bool sound_edge) {
    for (size_t i = 0; i < app->settings.behavior_shortcut_count; ++i) {
        BongoCatBehaviorShortcut *shortcut = &app->settings.behavior_shortcuts[i];
        for (size_t j = 0; j < app->behaviors.count; ++j) {
            BongoCatBehaviorEntry *behavior = &app->behaviors.entries[j];
            if (behavior->kind == BONGO_CAT_BEHAVIOR_SOUND) continue;
            if (strcmp(shortcut->id, behavior->id) != 0) continue;
    /* momentary 只认抬起, 且只有效果类需要收尾 (关掉覆盖层效果)。 */
            if (behavior->momentary &&
                bongo_cat_shortcut_release_matches(event, shortcut->shortcut)) {
                if (behavior->kind == BONGO_CAT_BEHAVIOR_EFFECT)
                    handled = bongo_cat_overlay_effect(app->overlay, NULL) || handled;
    /* 按下分支: 先确认快捷键状态机命中, 再让位给同组的可见动作。 */
            } else if (bongo_cat_shortcut_matches(&app->shortcut_state,
                event, shortcut->shortcut) &&
                !hidden_toggle_has_visible_binding(app, behavior,
                    shortcut->shortcut)) handled =
                        bongo_cat_app_run_behavior(app, behavior) || handled;
        }
    }
    if (handled) return true;
    /* 最后才是数字键别名: Alt+1..0 固定映射行为表前十条 (i==9 用 '0')。这是外部约定,
       但优先级在用户绑定之后, 所以同一个键可能同时触发两条路径。 */
    size_t limit = app->behaviors.count < 10 ? app->behaviors.count : 10;
    for (size_t i = 0; i < limit; ++i) {
        if (app->behaviors.entries[i].kind == BONGO_CAT_BEHAVIOR_SOUND && !sound_edge) continue;
        char alias[8];
        snprintf(alias, sizeof(alias), "Alt+%c", i == 9 ? '0' : (char)('1' + i));
        if (bongo_cat_shortcut_matches(&app->shortcut_state, event, alias))
            return bongo_cat_app_run_behavior(app, &app->behaviors.entries[i]);
    }
    return false;
}

/* 输入事件入口。sound_edge 是音效状态机的出参: 只有它变化过, 音效行为才允许被触发,
   否则每来一个不相干的按键都会重放音效。 */
void bongo_cat_app_shortcuts(BongoCatApp *app, const BongoCatInputEvent *event) {
    if (!app || !event) return;
    bool sound_edge = false;
    /* 先过音效快捷键: 它自己维护"按住"状态, 返回值与 sound_edge 一起决定后面要不要
       放过音效行为 (handled 只是提示, 不改变事件流)。 */
    bool sound_handled = bongo_cat_app_sound_shortcuts(app, event, &sound_edge);
    /* 手柄按键没有键盘修饰键状态可查, 只能匹配 "Gamepad:" 前缀的绑定; 它也不推进键盘
       快捷键状态机。 */
    if (event->kind == BONGO_CAT_INPUT_GAMEPAD_BUTTON) {
        behavior_shortcut(app, event, sound_handled, sound_edge);
        return;
    }
    /* primary == false 表示"主键没变化"(修饰键或重复按下), 这时只能试行为快捷键, 不要
       去比全局快捷键 —— 否则按住 Ctrl 时的每个键都会重放一遍全局动作。 */
    bool primary = bongo_cat_shortcut_update(&app->shortcut_state, event);
    if (!primary) {
        behavior_shortcut(app, event, sound_handled, sound_edge);
        return;
    }
    BongoCatShortcutPreferences *shortcuts = &app->settings.shortcuts;
    /* 全局快捷键命中后不再继续比对行为快捷键: 一次按下只做一件事, 避免"隐藏宠物"顺带
       触发一个表情。 */
    if (bongo_cat_shortcut_matches(&app->shortcut_state, event,
        shortcuts->toggle_pet_visibility)) {
        toggle_pet_visibility(app);
    } else if (bongo_cat_shortcut_matches(&app->shortcut_state, event,
        shortcuts->visible_preferences)) {
        bongo_cat_preferences_visible(app->preferences) ?
            bongo_cat_preferences_close(app->preferences) : bongo_cat_preferences_show(app->preferences);
    /* 镜像开关会改变模型锚点的语义 (锚点比例按窗口算), 所以必须让锚点失效重算。 */
    } else if (bongo_cat_shortcut_matches(&app->shortcut_state, event, shortcuts->mirror)) {
        app->settings.model.mirror = !app->settings.model.mirror;
        app->model_pointer_anchor_ready = false;
        app->dirty = true;
        bongo_cat_preferences_invalidate(app->preferences);
    /* 穿透开关直接改变命中判定与窗口属性, 改完要立刻让命中区失效并同步一次穿透。 */
    } else if (bongo_cat_shortcut_matches(&app->shortcut_state, event, shortcuts->pass_through)) {
        app->settings.window.pass_through = !app->settings.window.pass_through;
        bongo_cat_window_mark_hit_dirty(app);
        bongo_cat_window_sync_click_through(app);
        bongo_cat_preferences_invalidate(app->preferences);
    /* 置顶开关: 先改平台属性 (窗口句柄层), 再让命中区失效。 */
    } else if (bongo_cat_shortcut_matches(&app->shortcut_state, event,
        shortcuts->always_on_top)) {
        app->settings.window.always_on_top = !app->settings.window.always_on_top;
        bongo_cat_platform_set_always_on_top(&app->platform, app->settings.window.always_on_top);
        bongo_cat_window_mark_hit_dirty(app);
        bongo_cat_window_sync_click_through(app);
        bongo_cat_preferences_invalidate(app->preferences);
    } else behavior_shortcut(app, event, sound_handled, sound_edge);
}

/* 冒烟/自检辅助: 合成一个只有名字的输入事件走完整条快捷键路径。 */
static void test_key(BongoCatApp *app, BongoCatInputKind kind, const char *name) {
    BongoCatInputEvent event = {.kind = kind};
    snprintf(event.name, sizeof(event.name), "%s", name);
    bongo_cat_app_shortcuts(app, &event);
}

static void test_press(BongoCatApp *app, const char *name) {
    test_key(app, BONGO_CAT_INPUT_KEY_DOWN, name);
    test_key(app, BONGO_CAT_INPUT_KEY_UP, name);
}

/* 快捷键自检: 改写快捷键配置, 合成按下/抬起验证 5 条全局快捷键, 以及"按键抬起后状态能
   收敛" (修饰键不会卡在按下)。会临时改动 app->settings.shortcuts 与若干状态位, 只在
   冒烟模式调用。 */
bool bongo_cat_app_shortcuts_self_test(BongoCatApp *app) {
    if (!app || !app->preferences) return false;
    BongoCatShortcutPreferences *keys = &app->settings.shortcuts;
    snprintf(keys->toggle_pet_visibility,
        sizeof(keys->toggle_pet_visibility), "Control+B");
    snprintf(keys->visible_preferences, sizeof(keys->visible_preferences), "Control+Comma");
    snprintf(keys->mirror, sizeof(keys->mirror), "Control+M");
    snprintf(keys->pass_through, sizeof(keys->pass_through), "Control+P");
    snprintf(keys->always_on_top, sizeof(keys->always_on_top), "Control+T");
    app->session.window.visible = true;
    app->settings.model.mirror = false;
    app->model_pointer_anchor_ready = true;
    app->settings.window.pass_through = false;
    app->settings.window.always_on_top = false;
    test_key(app, BONGO_CAT_INPUT_KEY_DOWN, "ControlLeft");
    test_press(app, "KeyB");
    test_press(app, "KeyM");
    test_press(app, "KeyP");
    test_press(app, "KeyT");
    test_press(app, "Comma");
    test_key(app, BONGO_CAT_INPUT_KEY_UP, "ControlLeft");
    bool result = !app->session.window.visible && app->settings.model.mirror &&
        !app->model_pointer_anchor_ready &&
        app->settings.window.pass_through && app->settings.window.always_on_top &&
        bongo_cat_preferences_visible(app->preferences);
    bongo_cat_preferences_close(app->preferences);
    /* 自检期间必须关掉"忽略全局输入"标志: 否则 drain_input 会把合成事件丢掉, 末尾那条
       "按键抬起能收敛"的断言就永远不成立。 */

    bool ignored = app->smoke_ignore_global_input;
    app->smoke_ignore_global_input = false;
    test_key(app, BONGO_CAT_INPUT_KEY_DOWN, "ControlLeft");
    BongoCatInputEvent release = {.kind = BONGO_CAT_INPUT_KEY_UP};
    snprintf(release.name, sizeof(release.name), "ControlLeft");
    bongo_cat_input_push(&app->input, &release);
    bongo_cat_app_drain_input(app, false);
    bool mirror = app->settings.model.mirror;
    test_press(app, "KeyM");
    result = result && !app->shortcut_state.control &&
        app->settings.model.mirror == mirror;
    app->smoke_ignore_global_input = ignored;
    return result;
}
