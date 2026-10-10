/*
 * 输入消费点 (主循环每轮调用一次)。两个不变量:
 *   1. 队列里的事件必须全部 pop 出来逐条 apply, 不允许提前 break —— 丢掉一个按键抬起
 *      会让模型卡在按下状态;
 *   2. 副宠进程不接受快捷键, 也不能录快捷键 (allow_shortcuts 强制为 false)。
 * 快捷键动作与"按键状态推进"是两件事: 动作可以被抑制, 状态推进不能停 (见 drain_input
 * 里 else 分支的说明)。
 */
#include "runtime.h"
#include "bongo_cat/preferences.h"
#include "bongo_cat/shortcut.h"
#include "bongo_cat/log.h"

#include <string.h>

/* 10s 一次的输入诊断汇总 (节流)。排"按键没反应"这类问题时先看这一行: 每个计数对应
   一类丢事件的原因 (没 pop 到 / 被 CI 忽略 / 没映射到模型键 / 没有模型)。 */
static void log_input(BongoCatApp *app, uint64_t now) {
    if (now - app->input_diagnostics.log_ms < 10000) return;
    app->input_diagnostics.log_ms = now;
    int x = 0, y = 0;
    bool position_known = app->window && SDL_GetWindowPosition(app->window, &x, &y);
    SDL_DisplayID display = app->window ? SDL_GetDisplayForWindow(app->window) : 0;
    SDL_Rect bounds = {0};
    bool display_known = display && SDL_GetDisplayBounds(display, &bounds);
    SDL_LogInfo(BONGO_CAT_LOG_INPUT,
        "[input] consumer ticks_ms=%llu drained_keys=%llu ignored_keys=%llu "
        "mapped_keys=%llu unmapped_keys=%llu no_model_keys=%llu frames=%llu "
        "model_ready=%d visible=%d minimized=%d dirty=%d "
        "position_known=%d pet_position=%d,%d display_known=%d "
        "pet_display=%u pet_primary=%d pet_bounds=%d,%d,%d,%d",
        (unsigned long long)now,
        (unsigned long long)app->input_diagnostics.drained_keys,
        (unsigned long long)app->input_diagnostics.ignored_keys,
        (unsigned long long)app->input_diagnostics.mapped_keys,
        (unsigned long long)app->input_diagnostics.unmapped_keys,
        (unsigned long long)app->input_diagnostics.no_model_keys,
        (unsigned long long)app->input_diagnostics.presented_frames,
        app->live2d != NULL, app->session.window.visible, app->window_minimized,
        app->dirty, position_known, x, y, display_known, (unsigned)display,
        display && display == SDL_GetPrimaryDisplay(),
        bounds.x, bounds.y, bounds.w, bounds.h);
}

void bongo_cat_app_drain_input(BongoCatApp *app, bool allow_shortcuts) {
    if (app && app->secondary_pet) allow_shortcuts = false;
    BongoCatInputEvent event;
    while (bongo_cat_input_pop(&app->input, &event)) {
        bool keyboard = event.kind == BONGO_CAT_INPUT_KEY_DOWN ||
            event.kind == BONGO_CAT_INPUT_KEY_UP;
        if (keyboard) app->input_diagnostics.drained_keys++;
/* CI 用: 忽略全局键鼠输入, 免得跑测试的机器上有人敲键盘影响断言。 */
        if (app->smoke_ignore_global_input) {
            if (keyboard) app->input_diagnostics.ignored_keys++;
            continue;
        }
/* CapsLock 的按下/抬起在各平台上不好同步, 统一安排一次 100ms 后的合成抬起, 由下面
   take_scheduled_release 那一轮消费。 */
        if (strcmp(event.name, "CapsLock") == 0)
            bongo_cat_input_schedule_release(&app->input, &event, 100);
/* 任何会改变画面的输入都先终止窗口贴图快照, 让模型立刻响应; 拖拽中的鼠标事件例外,
   因为那时走的是窗口移动路径, 画面本来就该保持贴图。 */
        if (keyboard || event.kind == BONGO_CAT_INPUT_GAMEPAD_BUTTON ||
            event.kind == BONGO_CAT_INPUT_GAMEPAD_AXIS ||
            (!app->window_drag_active && (event.kind == BONGO_CAT_INPUT_MOUSE_DOWN ||
                event.kind == BONGO_CAT_INPUT_MOUSE_UP)))
            bongo_cat_window_snapshot_end(app);
        if (allow_shortcuts &&
            !bongo_cat_preferences_shortcuts_blocked(app->preferences))
            bongo_cat_app_shortcuts(app, &event);
        else {
            /* Suppress actions, not key transitions: releases may arrive
               while a shortcut is being recorded or a modal menu is open. */
            bongo_cat_shortcut_update(&app->shortcut_state, &event);
            if (app->sound_shortcut_state.count) {
                app->sound_shortcut_state.count = 0;
                memset(app->sound_shortcut_active, 0, sizeof(app->sound_shortcut_active));
            }
        }
        bongo_cat_app_apply_input(app, &event);
    }
/* 合成的抬起事件同样要走快捷键与 apply_input, 否则按下状态不会被清掉。 */
    uint64_t now = SDL_GetTicks();
    while (bongo_cat_input_take_scheduled_release(&app->input, now, &event)) {
        if (allow_shortcuts &&
            !bongo_cat_preferences_shortcuts_blocked(app->preferences))
            bongo_cat_app_shortcuts(app, &event);
        else bongo_cat_shortcut_update(&app->shortcut_state, &event);
        bongo_cat_app_apply_input(app, &event);
    }
/* 鼠标位置每轮都重算一次 (即使这轮没有任何事件): 指针可能被别的窗口或系统挪走了。 */
    if (!app->smoke_ignore_global_input) bongo_cat_app_apply_mouse(app);
    log_input(app, now);
}
