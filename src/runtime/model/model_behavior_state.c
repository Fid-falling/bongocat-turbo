/* 模型的"行为状态"记忆: 记录每个模型当前选中的持久动作与表情, 随 session.json 落盘, 下次加载
 * 该模型时再恢复 (见 bongo_cat_app_restore_behavior_state)。存储是 session.active_behaviors 里
 * 以 (model_id, behavior_id) 为键的扁平列表, 约定"尾部最新": 快照时先删该模型的旧条目再追加到
 * 尾部, 溢出则从头部丢弃 (上限 BONGO_CAT_BEHAVIOR_BINDING_CAP, 与快捷键数组共用)。
 */
#include "bongo_cat/app.h"

#include <stdio.h>
#include <string.h>

/* 条目此刻是否"生效中"。动作要求可见 (配对条目不算)、是开关型 (persistent) 且已被选中 ——
 * 一次性动作播完即过, 不该在下次启动被恢复; 表情只比序号。
 */
static bool behavior_active(const BongoCatApp *app,
    const BongoCatBehaviorEntry *entry, int expression) {
    if (entry->kind == BONGO_CAT_BEHAVIOR_MOTION) {
        if (!bongo_cat_live2d_motion_visible(app->live2d,
            entry->group, entry->index)) return false;
        return bongo_cat_live2d_motion_persistent(app->live2d,
            entry->group, entry->index) &&
            bongo_cat_live2d_motion_selected(app->live2d,
                entry->group, entry->index);
    }
    return entry->kind == BONGO_CAT_BEHAVIOR_EXPRESSION &&
        entry->index == expression;
}

/* 仅用于日志与诊断: 只数可见且选中的动作 (含一次性), 与快照保存的判据不同, 不要当成"会被恢复的动作数"。
 */
size_t bongo_cat_app_selected_motion_count(const BongoCatApp *app) {
    if (!app || !app->live2d) return 0;
    size_t count = 0;
    for (size_t i = 0; i < app->behaviors.count; ++i) {
        const BongoCatBehaviorEntry *entry = &app->behaviors.entries[i];
        if (entry->kind == BONGO_CAT_BEHAVIOR_MOTION &&
            bongo_cat_live2d_motion_visible(app->live2d,
                entry->group, entry->index) &&
            bongo_cat_live2d_motion_selected(app->live2d,
                entry->group, entry->index)) count++;
    }
    return count;
}

/* 原地压缩, 丢掉某个模型的全部条目并保持其余条目的相对顺序; count 先夹到容量上限,
 * 因为 session 可能来自旧版本写的文件。
 */
static void remove_model_state(BongoCatSessionState *session,
    const char *model_id) {
    size_t count = session->active_behavior_count;
    if (count > BONGO_CAT_BEHAVIOR_BINDING_CAP)
        count = BONGO_CAT_BEHAVIOR_BINDING_CAP;
    size_t output = 0;
    for (size_t i = 0; i < count; ++i) {
        BongoCatActiveBehavior entry = session->active_behaviors[i];
        if (strcmp(entry.model_id, model_id) != 0)
            session->active_behaviors[output++] = entry;
    }
    session->active_behavior_count = output;
}

/* 模型被删除或导入回滚时调用, 免得 session 里留下指向已不存在模型的条目。 */
void bongo_cat_app_forget_behavior_state(BongoCatApp *app,
    const char *model_id) {
    if (!app || !model_id || !model_id[0]) return;
    remove_model_state(&app->session, model_id);
}

/* 为马上要追加的 required 条腾位置: 只在真的超出容量时才从头部丢弃并搬移,
 * 尾部最新的约定不能被破坏 (恢复顺序依赖它)。
 */
static void make_room(BongoCatSessionState *session, size_t required) {
    size_t count = session->active_behavior_count;
    if (required > BONGO_CAT_BEHAVIOR_BINDING_CAP)
        required = BONGO_CAT_BEHAVIOR_BINDING_CAP;
    if (count + required <= BONGO_CAT_BEHAVIOR_BINDING_CAP) return;
    size_t remove = count + required - BONGO_CAT_BEHAVIOR_BINDING_CAP;
    memmove(session->active_behaviors, &session->active_behaviors[remove],
        (count - remove) * sizeof(session->active_behaviors[0]));
    session->active_behavior_count -= remove;
}

/* 快照当前模型的行为状态: 先删掉该模型已有条目, 再按目录顺序把生效中的追加到尾部,
 * 所以同一模型在列表里始终是一段连续区间。需要 live2d 就绪且已有加载中的模型。
 */
void bongo_cat_app_capture_behavior_state(BongoCatApp *app) {
    if (!app || !app->live2d || !app->loaded_model[0]) return;
    int expression = bongo_cat_live2d_expression(app->live2d);
    size_t active = 0;
    for (size_t i = 0; i < app->behaviors.count; ++i)
        if (behavior_active(app, &app->behaviors.entries[i], expression))
            active++;
    remove_model_state(&app->session, app->loaded_model);
    make_room(&app->session, active);
    for (size_t i = 0; i < app->behaviors.count &&
        app->session.active_behavior_count <
            BONGO_CAT_BEHAVIOR_BINDING_CAP; ++i) {
        const BongoCatBehaviorEntry *entry = &app->behaviors.entries[i];
        if (!behavior_active(app, entry, expression)) continue;
        BongoCatActiveBehavior *saved = &app->session.active_behaviors[
            app->session.active_behavior_count++];
        memset(saved, 0, sizeof(*saved));
        snprintf(saved->model_id, sizeof(saved->model_id), "%s",
            app->loaded_model);
        snprintf(saved->behavior_id, sizeof(saved->behavior_id), "%s",
            entry->id);
    }
}

size_t bongo_cat_app_saved_behavior_count(const BongoCatApp *app,
    const char *model_id) {
    if (!app || !model_id) return 0;
    size_t limit = app->session.active_behavior_count;
    if (limit > BONGO_CAT_BEHAVIOR_BINDING_CAP)
        limit = BONGO_CAT_BEHAVIOR_BINDING_CAP;
    size_t count = 0;
    for (size_t i = 0; i < limit; ++i)
        if (strcmp(app->session.active_behaviors[i].model_id, model_id) == 0)
            count++;
    return count;
}

/* 在"当前"模型的行为目录里按 id 查找; 返回借用指针, 换模型后失效。 */
static const BongoCatBehaviorEntry *find_behavior(const BongoCatApp *app,
    const char *id) {
    for (size_t i = 0; i < app->behaviors.count; ++i)
        if (strcmp(app->behaviors.entries[i].id, id) == 0)
            return &app->behaviors.entries[i];
    return NULL;
}

/* 加载或切换模型之后调用: 只恢复新目录里仍存在、且当前未被选中的持久动作与表情,
 * 已经选中的不再重复触发 (避免打断正在播放的动作)。
 */
void bongo_cat_app_restore_behavior_state(BongoCatApp *app,
    const char *model_id) {
    if (!app || !app->live2d || !model_id || !model_id[0]) return;
    size_t count = app->session.active_behavior_count;
    if (count > BONGO_CAT_BEHAVIOR_BINDING_CAP)
        count = BONGO_CAT_BEHAVIOR_BINDING_CAP;
    for (size_t i = 0; i < count; ++i) {
        const BongoCatActiveBehavior *saved =
            &app->session.active_behaviors[i];
        if (strcmp(saved->model_id, model_id) != 0) continue;
        const BongoCatBehaviorEntry *entry = find_behavior(app,
            saved->behavior_id);
        if (!entry) continue;
        if (entry->kind == BONGO_CAT_BEHAVIOR_MOTION &&
            bongo_cat_live2d_motion_visible(app->live2d,
                entry->group, entry->index) &&
            bongo_cat_live2d_motion_persistent(app->live2d,
                entry->group, entry->index) &&
            !bongo_cat_live2d_motion_selected(app->live2d,
                entry->group, entry->index))
            bongo_cat_live2d_restore_motion_state(app->live2d,
                entry->group, entry->index);
        else if (entry->kind == BONGO_CAT_BEHAVIOR_EXPRESSION)
            bongo_cat_live2d_set_expression(app->live2d, entry->index);
    }
}
