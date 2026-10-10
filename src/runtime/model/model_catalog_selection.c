/* 目录变化后的选中项修复（reconcile）与多宠选择快照。目录重扫、删除、后台刷新完成后都会
 * 调用 bongo_cat_model_catalog_reconcile()，它必须容忍表里找不到当前选中或已加载的 id，并
 * 把多宠附加列表收敛到仍然存在的模型上；返回值表示是否改动过会话状态，调用方据此决定要不
 * 要通知偏好设置窗口。 */
#include "model_catalog_selection.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

/* 纯读取，快照多宠附加列表；count 先夹到 BONGO_CAT_ADDITIONAL_MODEL_CAP，免得会话状态被
 * 外部写坏后这里越界拷贝。返回纯值拷贝，没有堆所有权转移。 */
BongoCatModelSelection bongo_cat_model_selection_capture(
    const BongoCatApp *app) {
    BongoCatModelSelection selection = {0};
    if (!app) return selection;
    selection.count = app->session.additional_model_count;
    if (selection.count > BONGO_CAT_ADDITIONAL_MODEL_CAP)
        selection.count = BONGO_CAT_ADDITIONAL_MODEL_CAP;
    memcpy(selection.additional, app->session.additional_model_ids,
        selection.count * sizeof(selection.additional[0]));
    return selection;
}

/* 先清空再按快照重加，但只恢复当前目录里仍然存在的 id —— 快照可能比目录新（模型刚被删），
 * 照抄会把死 id 塞回会话。最后重算多宠主模型，时间戳只能用当前 tick。 */
void bongo_cat_model_selection_restore(BongoCatApp *app,
    const BongoCatModelSelection *selection) {
    if (!app || !selection) return;
    bongo_cat_session_clear_additional_models(&app->session);
    for (size_t i = 0; i < selection->count; ++i)
        if (bongo_cat_models_find(&app->models, selection->additional[i]))
            bongo_cat_session_add_model(&app->session,
                selection->additional[i]);
    bongo_cat_multi_pet_primary_update(app, SDL_GetTicksNS());
}

/* 没有已知选中项时的默认模型：优先内置三件套 standard、keyboard、gamepad，都没有就退回目
 * 录第一条；返回借用指针（目录为空时 NULL），不转移所有权。 */
static const BongoCatModelEntry *fallback_model(
    const BongoCatModelCatalog *models) {
    static const char *const ids[] = {"standard", "keyboard", "gamepad"};
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
        const BongoCatModelEntry *entry = bongo_cat_models_find(models, ids[i]);
        if (entry) return entry;
    }
    return models && models->count ? &models->entries[0] : NULL;
}

/* 会话里的附加列表是否与快照完全一致（数量加逐项 id）；reconcile 用它判断修复有没有真正改
 * 动状态，从而决定要不要通知偏好设置窗口。 */
static bool selection_matches(const BongoCatApp *app,
    const BongoCatModelSelection *selection) {
    if (!app || !selection || app->session.additional_model_count !=
            selection->count) return false;
    return !memcmp(app->session.additional_model_ids, selection->additional,
        selection->count * sizeof(selection->additional[0]));
}

/* 进入无模型可显示状态：清掉 loaded 与 loading 的 id 及模式，并关掉手柄输入 —— 留着
 * gamepad 模式会让下一帧继续把手柄参数喂给一个已经不存在的模型。 */
static void clear_loaded_model(BongoCatApp *app) {
    if (!app) return;
    app->loaded_model[0] = '\0';
    app->loading_model[0] = '\0';
    app->loaded_mode = BONGO_CAT_MODE_STANDARD;
    bongo_cat_gamepads_set_enabled(app, false);
}

/* 修复目录变化后的选中状态，返回是否改动过任何会话字段。修复顺序是有意的：先补一个有效的
 * 主模型（已加载的优先，再退到内置默认），再收敛多宠列表，最后处理已加载模型消失的情况
 * —— 只有存在替代模型且渲染器就绪时才重新加载，否则清空加载状态，让主循环走无模型分支，
 * 而不是继续用一个悬空的 Live2D 状态。 */
bool bongo_cat_model_catalog_reconcile(BongoCatApp *app) {
    if (!app) return false;
    BongoCatModelSelection selection =
        bongo_cat_model_selection_capture(app);
    const BongoCatModelEntry *active = bongo_cat_models_find(&app->models,
        app->session.active_model_id);
    const BongoCatModelEntry *loaded = bongo_cat_models_find(&app->models,
        app->loaded_model);
    bool changed = false;
    if (!active) {
        active = loaded ? loaded : fallback_model(&app->models);
        if (active) {
            if (strcmp(app->session.active_model_id, active->id) != 0) {
                snprintf(app->session.active_model_id,
                    sizeof(app->session.active_model_id), "%s", active->id);
                changed = true;
            }
        } else if (app->session.active_model_id[0]) {
            app->session.active_model_id[0] = '\0';
            changed = true;
        }
    }
    if (app->settings.model.multiple_pets) {
        bongo_cat_model_selection_restore(app, &selection);
        if (!selection_matches(app, &selection)) changed = true;
    } else if (app->session.additional_model_count) {
        bongo_cat_session_clear_additional_models(&app->session);
        changed = true;
    }
    if (app->loaded_model[0] && !loaded) {
        if (active && app->live2d) {
            BongoCatError error = {0};
            if (!bongo_cat_app_select_model_with_error(app, active->id,
                    &error)) {
                if (error.message[0]) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to restore an available model after refresh: %s",
                    error.message);
                clear_loaded_model(app);
                changed = true;
            }
        } else {
            clear_loaded_model(app);
            changed = true;
        }
    }
    if (!active && app->session.additional_model_count) {
        bongo_cat_session_clear_additional_models(&app->session);
        changed = true;
    }
    return changed;
}
