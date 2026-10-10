/*
 * 音效行为的按键状态机: 音效绑定可以"按住播放/松开停止", 所以需要单独跟踪每个物理键的
 * 按住状态 (BongoCatSoundShortcutState), 而不是复用键盘快捷键那套"修饰键 + 单主键"状态。
 * 调用方: shortcuts.c 在每次输入事件时先调本函数 —— *changed 为假就说明这次事件没有改变
 * 任何按住状态, 后面不必再试音效行为。
 * 数组下标是"绑定在 settings.behavior_shortcuts 里的位置", 与 app->sound_shortcut_active
 * 一一对应; 绑定表被重排或截断后调用方必须清掉这些标记 (见 app_input.c 与
 * bongo_cat_app_reset_gamepad), 否则会出现"松开却不停止"。
 */
#include "runtime.h"

#include <string.h>

/* 更新按住状态并触发这一事件里"从松到按"的音效行为。*changed 由状态机给出: 没有边沿变化
   时直接返回 false, 不做任何行为匹配。返回值表示"当前是否仍有音效键按住"。 */
bool bongo_cat_app_sound_shortcuts(BongoCatApp *app, const BongoCatInputEvent *event,
    bool *changed) {
    /* 只需处理发生了边沿变化的事件: 状态机内部会过滤重复按下。 */
    *changed = bongo_cat_sound_shortcut_update(&app->sound_shortcut_state, event);
    if (!*changed) return false;
    bool handled = false;
    const BongoCatBehaviorEntry *pending[BONGO_CAT_BEHAVIOR_BINDING_CAP];
    size_t pending_count = 0;
    /* 收集本事件的待触发项后统一执行: 不能在遍历绑定时直接执行行为 —— 行为可能改动配置
       (例如切换模型), 那会让正在使用的绑定指针失效。 */
    for (size_t i = 0; i < app->settings.behavior_shortcut_count; ++i) {
        const BongoCatBehaviorShortcut *binding = &app->settings.behavior_shortcuts[i];
        for (size_t j = 0; j < app->behaviors.count; ++j) {
            const BongoCatBehaviorEntry *entry = &app->behaviors.entries[j];
            if (entry->kind != BONGO_CAT_BEHAVIOR_SOUND || strcmp(binding->id, entry->id)) continue;
            bool down = bongo_cat_sound_shortcut_down(&app->sound_shortcut_state, binding->shortcut);
            handled = handled || down;
    /* 只有"从未按住变成按住"才触发一次; active[] 记的是上一轮的状态, 用它做边沿判定。 */
            if (down && !app->sound_shortcut_active[i] &&
                pending_count < BONGO_CAT_BEHAVIOR_BINDING_CAP) pending[pending_count++] = entry;
            app->sound_shortcut_active[i] = down;
        }
    }
    /* 行为在收集完成之后再执行, 遍历过程中不调用任何会改动配置的接口。 */
    for (size_t i = 0; i < pending_count; ++i) bongo_cat_app_run_behavior(app, pending[i]);
    return handled;
}
