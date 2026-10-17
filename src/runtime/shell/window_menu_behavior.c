/* 右键菜单里的"行为"层: 把 app->behaviors 的条目 (动作 motion / 表情 expression / 声音
 * sound) 映射成菜单项、标签与勾选态, 并决定选中之后怎么执行。
 * 三处最容易改错: 1) 菜单动作里的序号是"按可见性过滤之后第 N 个", 不是 entries 下标,
 * 换算一律走 nth_behavior, 改过滤条件时必须同步改 action 与 labels 两处; 2) 共享同一
 * 快捷键的条目要一起执行, 但配对的隐藏项要跳过; 3) 执行语义以 model_behavior.c 为准 ——
 * 返回 false 表示这次没生效, 调用方不要回滚已经改变的状态。 */
#include "window_menu.h"
#include "model_sound.h"
#include "bongo_cat/audio.h"
#include "bongo_cat/i18n.h"
#include "bongo_cat/shortcut.h"

#include <stdio.h>
#include <string.h>

/* 按"过滤后的序号"取条目: 声音要求有 sound 或 sound_clear, 动作要求当前模型里可见
 * (不可见的是配对项), 表情不过滤。position 从 0 起算, 越界返回 NULL; 返回值是借用的
 * entries 指针, 模型或动作表重建后立即失效。 */
static const BongoCatBehaviorEntry *nth_behavior(BongoCatApp *app,
    BongoCatBehaviorKind kind, size_t position) {
    if (!app) return NULL;
    for (size_t i = 0; i < app->behaviors.count; ++i) {
        const BongoCatBehaviorEntry *entry = &app->behaviors.entries[i];
        if (entry->kind != kind) continue;
        if (kind == BONGO_CAT_BEHAVIOR_SOUND && !entry->sound[0] && !entry->sound_clear) continue;
        if (kind == BONGO_CAT_BEHAVIOR_MOTION &&
            !bongo_cat_live2d_motion_visible(app->live2d,
                entry->group, entry->index)) continue;
        if (!position) return entry;
        position--;
    }
    return NULL;
}

/* 查该条目在用户绑定表里的快捷键; 未绑定或 app/id 为空返回 NULL。返回借用指针,
 * 指向 settings.behavior_shortcuts, 调用方不要 free。 */
static const char *behavior_shortcut(const BongoCatApp *app, const char *id) {
    if (!app || !id) return NULL;
    for (size_t i = 0; i < app->settings.behavior_shortcut_count; ++i) {
        const BongoCatBehaviorShortcut *binding = &app->settings.behavior_shortcuts[i];
        if (strcmp(binding->id, id) == 0) return binding->shortcut;
    }
    return NULL;
}

/* 有一类动作条目是"配对项": 与另一个动作共用同一快捷键, 代表开/关两侧, 因此不可见
 * (见 model.h 的 motion_visible 契约)。执行某动作时不能顺手执行它的隐藏配对项 —— 那等于
 * 立刻把自己关掉; 所以这里先确认该隐藏项没有别的可见条目共用同一快捷键。 */
static bool hidden_toggle_has_visible_binding(BongoCatApp *app,
    const BongoCatBehaviorEntry *entry, const char *shortcut) {
    if (!app || !entry || !shortcut || !shortcut[0] ||
        entry->kind != BONGO_CAT_BEHAVIOR_MOTION ||
        bongo_cat_live2d_motion_visible(app->live2d,
            entry->group, entry->index)) return false;
    for (size_t i = 0; i < app->behaviors.count; ++i) {
        const BongoCatBehaviorEntry *candidate = &app->behaviors.entries[i];
        const char *bound = behavior_shortcut(app, candidate->id);
        if (candidate != entry &&
            candidate->kind == BONGO_CAT_BEHAVIOR_MOTION &&
            bongo_cat_live2d_motion_visible(app->live2d,
                candidate->group, candidate->index) &&
            bongo_cat_live2d_motion_same_toggle(app->live2d,
                entry->group, entry->index,
                candidate->group, candidate->index) &&
            bound && strcmp(bound, shortcut) == 0) return true;
    }
    return false;
}

/* 文案优先级: 用户自定义绑定标签 > sound_clear 的内置 i18n 文案 > 条目自带 label。
 * 返回借用指针 (可能是 i18n 常量), 生命周期到下次设置重载为止。 */
static const char *behavior_label(const BongoCatApp *app,
    const BongoCatBehaviorEntry *entry) {
    if (!app || !entry) return "";
    for (size_t i = 0; i < app->settings.behavior_shortcut_count; ++i) {
        const BongoCatBehaviorShortcut *binding = &app->settings.behavior_shortcuts[i];
        if (strcmp(binding->id, entry->id) == 0 && binding->label[0])
            return binding->label;
    }
    if (entry->sound_clear) return bongo_cat_i18n_get(app->i18n,
        "pages.preference.model.behaviorModal.labels.stopAllAudio", "Stop all audio");
    return entry->label;
}

/* 组装 "标签 - 快捷键" 文案 (没绑定快捷键就只写标签), 用 snprintf 截断在
 * BONGO_CAT_MENU_LABEL_CAP; entry 必须非 NULL, shortcut_label 是格式化后的可读名。 */
static void menu_label(char output[BONGO_CAT_MENU_LABEL_CAP],
    const BongoCatApp *app, const BongoCatBehaviorEntry *entry) {
    const char *label = behavior_label(app, entry);
    const char *shortcut = behavior_shortcut(app, entry->id);
    char shortcut_label[BONGO_CAT_SHORTCUT_CAP * 2];
    bongo_cat_shortcut_format(shortcut, shortcut_label, sizeof(shortcut_label));
    if (shortcut && shortcut[0]) snprintf(output, BONGO_CAT_MENU_LABEL_CAP,
        "%s - %s", label, shortcut_label);
    else snprintf(output, BONGO_CAT_MENU_LABEL_CAP, "%s", label);
}

/* 执行选中条目, 并把共享同一快捷键的其它条目一并执行 —— 点一次菜单与按一次快捷键必须得到
 * 同一组效果; 隐藏配对项除外 (见 hidden_toggle_has_visible_binding)。返回"是否至少有一项
 * 生效", 单项失败不回滚前面已成功的项。 */
static bool run_binding(BongoCatApp *app, const BongoCatBehaviorEntry *selected) {
    const char *shortcut = behavior_shortcut(app, selected->id);
    bool handled = bongo_cat_app_run_behavior(app, selected);
    if (!shortcut || !shortcut[0]) return handled;
    for (size_t i = 0; i < app->behaviors.count; ++i) {
        const BongoCatBehaviorEntry *entry = &app->behaviors.entries[i];
        const char *bound = behavior_shortcut(app, entry->id);
        if (entry != selected && bound && strcmp(bound, shortcut) == 0 &&
            !hidden_toggle_has_visible_binding(app, entry, shortcut))
            handled = bongo_cat_app_run_behavior(app, entry) || handled;
    }
    return handled;
}

/* 导出菜单要用的标签: 两个数组容量都必须 >= BONGO_CAT_BEHAVIOR_CAP, 下标与
 * BONGO_CAT_MENU_MOTION_FIRST / EXPRESSION_FIRST 的偏移一一对应 (菜单动作即下标)。
 * 三个数组指针都允许为 NULL; motion_count/expression_count 必须非空, 否则直接返回。 */
void bongo_cat_window_behavior_labels(BongoCatApp *app,
    char motions[][BONGO_CAT_MENU_LABEL_CAP], bool *motion_checked,
    size_t *motion_count, char expressions[][BONGO_CAT_MENU_LABEL_CAP],
    size_t *expression_count,
    size_t *current_expression) {
    if (!motion_count || !expression_count) return;
    *motion_count = 0; *expression_count = 0;
    if (current_expression) *current_expression = BONGO_CAT_BEHAVIOR_CAP;
    /* 哨兵 BONGO_CAT_BEHAVIOR_CAP = 当前没有选中任何表情, 与"第 0 项被选中"必须区分。 */
    if (!app) return;
    int active_expression = bongo_cat_live2d_expression(app->live2d);
    for (size_t i = 0; i < app->behaviors.count; ++i) {
        const BongoCatBehaviorEntry *entry = &app->behaviors.entries[i];
        if (entry->kind == BONGO_CAT_BEHAVIOR_MOTION && motions) {
            if (!bongo_cat_live2d_motion_visible(app->live2d,
                entry->group, entry->index)) continue;
            if (motion_checked) motion_checked[*motion_count] =
                bongo_cat_live2d_motion_selected(app->live2d,
                    entry->group, entry->index);
            menu_label(motions[*motion_count], app, entry);
            (*motion_count)++;
        } else if (entry->kind == BONGO_CAT_BEHAVIOR_EXPRESSION && expressions) {
            if (current_expression && entry->index == active_expression)
                *current_expression = *expression_count;
            menu_label(expressions[*expression_count], app, entry);
            (*expression_count)++;
        }
    }
}

/* 菜单动作 -> 行为执行: 动作/表情按"过滤后序号"取条目再走 run_binding; 音频条目比较特殊,
 * 同一条目在播放中是停止、否则才是播放。返回 false = 这次没执行 (序号越界或条目不存在),
 * 调用方据此决定是否还要补画一帧。 */
bool bongo_cat_window_behavior_action(BongoCatApp *app,
    BongoCatMenuAction action) {
    BongoCatBehaviorKind kind;
    size_t position;
    if (action >= BONGO_CAT_MENU_MOTION_FIRST &&
        action < BONGO_CAT_MENU_MOTION_FIRST + BONGO_CAT_BEHAVIOR_CAP) {
        kind = BONGO_CAT_BEHAVIOR_MOTION;
        position = (size_t)(action - BONGO_CAT_MENU_MOTION_FIRST);
    } else if (action >= BONGO_CAT_MENU_EXPRESSION_FIRST &&
        action < BONGO_CAT_MENU_EXPRESSION_FIRST + BONGO_CAT_BEHAVIOR_CAP) {
        kind = BONGO_CAT_BEHAVIOR_EXPRESSION;
        position = (size_t)(action - BONGO_CAT_MENU_EXPRESSION_FIRST);
    } else if (action >= BONGO_CAT_MENU_AUDIO_FIRST &&
        action < BONGO_CAT_MENU_AUDIO_FIRST + BONGO_CAT_BEHAVIOR_CAP) {
        const BongoCatBehaviorEntry *sound = nth_behavior(app, BONGO_CAT_BEHAVIOR_SOUND,
            (size_t)(action - BONGO_CAT_MENU_AUDIO_FIRST));
        if (!sound) return false;
        /* 音频条目是开关语义: 正在播就停, 否则走通用执行路径 (含绑定音效与状态回写)。 */
        if (bongo_cat_audio_is_playing(app->audio, sound->sound))
            bongo_cat_audio_stop_path(app->audio, sound->sound);
        else return bongo_cat_app_run_behavior(app, sound);
        return true;
    } else return false;
    const BongoCatBehaviorEntry *entry = nth_behavior(app, kind, position);
    return entry && run_binding(app, entry);
}

/* 预览事务的"试穿": 菜单打开期间每次 hover 到条目调一次, 结束时必须 commit 或 restore
 * 回滚 (见 window_menu_preview.c)。动作走 live2d 的预览事务; 表情是切换语义 —— 再预览
 * 同一项即取消 (传 -1 走淡出清除), 所以这里比较当前表情再决定 target。 */
bool bongo_cat_window_behavior_preview(BongoCatApp *app,
    BongoCatMenuAction action) {
    if (!app) return false;
    if (action >= BONGO_CAT_MENU_MOTION_FIRST &&
        action < BONGO_CAT_MENU_MOTION_FIRST + BONGO_CAT_BEHAVIOR_CAP) {
        size_t position = (size_t)(action - BONGO_CAT_MENU_MOTION_FIRST);
        const BongoCatBehaviorEntry *entry = nth_behavior(app,
            BONGO_CAT_BEHAVIOR_MOTION, position);
        return entry && bongo_cat_live2d_preview_motion(app->live2d,
            entry->group, entry->index);
    }
    if (action >= BONGO_CAT_MENU_EXPRESSION_FIRST &&
        action < BONGO_CAT_MENU_EXPRESSION_FIRST + BONGO_CAT_BEHAVIOR_CAP) {
        size_t position = (size_t)(action - BONGO_CAT_MENU_EXPRESSION_FIRST);
        const BongoCatBehaviorEntry *entry = nth_behavior(app,
            BONGO_CAT_BEHAVIOR_EXPRESSION, position);
        if (!entry) return false;
        int target = bongo_cat_live2d_expression(app->live2d) == entry->index ?
            -1 : entry->index;
        return bongo_cat_live2d_set_expression(app->live2d, target);
    }
    return false;
}

/* 预览落定: 提交动作预览 -> 播放该动作绑定的音效 (失败只记日志, 不回滚动作) -> 执行共享
 * 同一快捷键的可见条目 -> 把可见状态写回会话并置脏。只处理动作 (motion) 类, 表情由调用方
 * 自己 capture。返回 false 表示什么都没提交 (序号越界或动作不存在)。 */
bool bongo_cat_window_behavior_commit_preview(BongoCatApp *app,
    BongoCatMenuAction action) {
    if (!app || action < BONGO_CAT_MENU_MOTION_FIRST ||
        action >= BONGO_CAT_MENU_MOTION_FIRST + BONGO_CAT_BEHAVIOR_CAP)
        return false;
    const BongoCatBehaviorEntry *entry = nth_behavior(app,
        BONGO_CAT_BEHAVIOR_MOTION,
        (size_t)(action - BONGO_CAT_MENU_MOTION_FIRST));
    if (!entry || !bongo_cat_live2d_commit_motion_preview(app->live2d,
        entry->group, entry->index)) return false;
    if (entry->sound[0])
        bongo_cat_app_play_sound(app, entry->sound, false);
    const char *shortcut = behavior_shortcut(app, entry->id);
    if (shortcut && shortcut[0]) for (size_t i = 0; i < app->behaviors.count; ++i) {
        const BongoCatBehaviorEntry *companion = &app->behaviors.entries[i];
        const char *bound = behavior_shortcut(app, companion->id);
        if (companion != entry && bound && strcmp(bound, shortcut) == 0 &&
            !hidden_toggle_has_visible_binding(app, companion, shortcut))
            bongo_cat_app_run_behavior(app, companion);
    }
    bongo_cat_app_capture_behavior_state(app);
    app->dirty = true;
    return true;
}

bool bongo_cat_window_behavior_menu_action(BongoCatMenuAction action) {
    return (action >= BONGO_CAT_MENU_MOTION_FIRST &&
        action < BONGO_CAT_MENU_MOTION_FIRST + BONGO_CAT_BEHAVIOR_CAP) ||
        (action >= BONGO_CAT_MENU_EXPRESSION_FIRST &&
            action < BONGO_CAT_MENU_EXPRESSION_FIRST + BONGO_CAT_BEHAVIOR_CAP);
}

/* 音频分组: names/checked 容量都必须是 BONGO_CAT_BEHAVIOR_CAP, count 由本函数写出; 全部条目
 * 都没有真实音频文件时把 count 归零 (只有 sound_clear 的分组不显示)。app 不可为 NULL。 */
void bongo_cat_window_audio_labels(BongoCatApp *app,
    char names[][BONGO_CAT_MENU_LABEL_CAP], bool *checked, size_t *count) {
    *count = 0;
    bool available = false;
    for (size_t i = 0; i < app->behaviors.count; ++i) {
        const BongoCatBehaviorEntry *entry = &app->behaviors.entries[i];
        if (entry->kind != BONGO_CAT_BEHAVIOR_SOUND || (!entry->sound[0] && !entry->sound_clear)) continue;
        available = available || entry->sound[0];
        menu_label(names[*count], app, entry);
        checked[*count] = bongo_cat_audio_is_playing(app->audio, entry->sound);
        ++*count;
    }
    if (!available) *count = 0;
}
