/*
 * 行为重命名 (行左半部分的名字)。
 *
 * 名字存的地方不是行为表而是 settings.behavior_shortcuts 里的 label 字段 —— 行为表
 * 是每次启动从模型里枚举出来的, 改它下次启动就没了; 绑定记录才是持久的。所以
 * "恢复默认"的写法是: 当新名字恰好等于原始名时, 存成空字符串 (空 = 用原始名),
 * 而不是把名字原样存一份。见 rename_finish 里的 label 计算。
 *
 * 改名会引入新字形, 所以结束时要重建字体图集 (reload_fonts) —— 不重建的话用户
 * 刚打进去的汉字会显示成方块。
 */
#include "preferences_state.h"
#include "preferences_text_edit.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

/*
 * 与 preferences_behavior_row.c 里的同名函数是一份重复实现: 两边都按需惰性创建绑定
 * 记录, 所以逻辑必须保持一致 (改动时两处一起改)。
 */
static BongoCatBehaviorShortcut *binding_for(BongoCatSettings *config,
    const char *id) {
    for (size_t i = 0; i < config->behavior_shortcut_count; ++i)
        if (!strcmp(config->behavior_shortcuts[i].id, id))
            return &config->behavior_shortcuts[i];
    if (config->behavior_shortcut_count >= BONGO_CAT_BEHAVIOR_BINDING_CAP)
        return NULL;
    BongoCatBehaviorShortcut *binding =
        &config->behavior_shortcuts[config->behavior_shortcut_count++];
    memset(binding, 0, sizeof(*binding));
    snprintf(binding->id, sizeof(binding->id), "%s", id);
    return binding;
}

static const char *display_label(const BongoCatBehaviorEntry *entry,
    const BongoCatBehaviorShortcut *binding) {
    return binding && binding->label[0] ? binding->label : entry->label;
}

/*
 * 结束改名。save=false 是 ESC/取消, 这时什么都不写。
 * 只有在"新名字真的和旧的不一样"时才重建字体图集 —— 每次回车都重建一次图集
 * 会让对话框明显卡一下。
 */
void bongo_cat_preferences_behavior_rename_finish(
    BongoCatPreferences *value, bool save) {
    if (!value || !bongo_cat_preferences_text_session_active(
        &value->behavior_rename)) return;
    BongoCatPreferencesTextSession *session = &value->behavior_rename;
    bool label_changed = false;
    if (save) {
        bongo_cat_text_edit_trim(session->text);
        BongoCatBehaviorEntry *entry = NULL;
        for (size_t i = 0; i < value->app->behaviors.count; ++i)
            if (!strcmp(value->app->behaviors.entries[i].id, session->id)) {
                entry = &value->app->behaviors.entries[i];
                break;
            }
        BongoCatBehaviorShortcut *binding = binding_for(&value->app->settings,
            session->id);
        if (binding) {
            const char *label = entry && !strcmp(session->text, entry->label) ?
                "" : session->text;
            label_changed = strcmp(binding->label, label) != 0;
            snprintf(binding->label, sizeof(binding->label), "%s", label);
        }
    }
    bongo_cat_preferences_text_session_reset(session);
    SDL_StopTextInput(value->window);
    if (label_changed) bongo_cat_preferences_reload_fonts(value);
    value->render_dirty = true;
}

/*
 * 开始改名。先 cancel 掉快捷键录制: 两个会话都要独占键盘, 同时开着会互相抢文字输入
 * (rename 吃 TEXT_INPUT, 录制吃 KEY_DOWN, 用户会看到两处同时在变)。
 */
void bongo_cat_preferences_behavior_rename_begin(BongoCatPreferences *value,
    const BongoCatBehaviorEntry *entry, BongoCatBehaviorShortcut *binding,
    struct nk_rect bounds) {
    if (!value || !entry) return;
    bongo_cat_preferences_shortcut_cancel(value);
    bongo_cat_preferences_text_session_begin(&value->behavior_rename,
        entry->id, display_label(entry, binding), bounds);
    SDL_StartTextInput(value->window);
    value->render_dirty = true;
}

/* 文本编辑会话的事件入口; 这里只把 result 翻译成"结束并保存/放弃"和"要重画" */
bool bongo_cat_preferences_behavior_rename_event(
    BongoCatPreferences *value, const SDL_Event *event) {
    if (!value) return false;
    BongoCatPreferencesTextSessionEvent result =
        bongo_cat_preferences_text_session_event(&value->behavior_rename,
            event, value->ui.layout_scale, value->ui.caption_font, 8.0f);
    if (result.finish)
        bongo_cat_preferences_behavior_rename_finish(value, result.save);
    if (result.handled) value->render_dirty = true;
    return result.handled;
}
