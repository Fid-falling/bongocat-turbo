/* "哪些模型显示在桌面上"的选中逻辑 (多桌宠开关背后的语义)。未开启多桌宠时它就是单选模型,
 * 开启后第一个是主实例、其余各跑一个子进程; 主模型被取消时要提升一个副模型顶上,
 * 并保留它原本的屏幕位置与透明度。
 */
#include "runtime.h"
#include "bongo_cat/path.h"

#include <stdio.h>
#include <string.h>

/* 读副模型自己写的 session.json 里的窗口状态 (子进程的 state_root 就是 pets/<hash> 目录);
 * position_known 为假就不算数 —— 提升为主实例时不能拿一个没意义的默认值当真。
 */
static bool load_pet_window(const BongoCatApp *app, const char *model_id,
    BongoCatWindowState *window) {
    char directory[BONGO_CAT_PATH_CAP], path[BONGO_CAT_PATH_CAP];
    BongoCatSessionState session;
    BongoCatError error = {0};
    bongo_cat_session_defaults(&session);
    if (!bongo_cat_multi_pet_state_directory(directory, sizeof(directory),
            app->primary_state_root, model_id) ||
        !bongo_cat_path_join(path, sizeof(path), directory, "session.json") ||
        bongo_cat_session_load(path, &session, &error) != BONGO_CAT_OK ||
        !session.window.position_known) return false;
    *window = session.window;
    return true;
}

/* 从副桌宠提升为主实例后: 搬回原屏幕位置、同步透明度并清掉 hover 淡出的中间态, 免得新实例继承旧状态。
 */
static void adopt_pet_window(BongoCatApp *app,
    const BongoCatWindowState *window) {
    if (!bongo_cat_window_apply_geometry(app, window->x, window->y,
            window->scale_percent, window->width, window->height)) return;
    app->session.window.opacity_percent = window->opacity_percent;
    bongo_cat_app_cancel_hover_fade(app);
    app->hover_hidden = app->hover_inside = false;
    app->hover_deadline_ns = 0;
    bongo_cat_platform_set_opacity(&app->platform,
        window->opacity_percent / 100.0f);
}

bool bongo_cat_app_model_active(const BongoCatApp *app, const char *id) {
    if (!app || !id) return false;
    if (!strcmp(app->session.active_model_id, id)) return true;
    return app->settings.model.multiple_pets &&
        bongo_cat_session_model_active(&app->session, id);
}

size_t bongo_cat_app_active_model_count(const BongoCatApp *app) {
    return app && app->settings.model.multiple_pets
        ? 1 + app->session.additional_model_count : app ? 1 : 0;
}

/* 勾选或取消一个模型。失败经 error 返回: 模型未安装, 或副模型数量已达上限; 未开启多桌宠时
 * active=true 就是切换当前模型, active=false 无事可做。取消的若是主模型而桌上还有副模型:
 * 先记下第一个副模型的窗口状态, 把它提升为主实例, 再从副列表里摘掉, 最后搬回原窗口位置。
 */
bool bongo_cat_app_set_model_active(BongoCatApp *app, const char *id,
    bool active, BongoCatError *error) {
    if (!app || !id || !bongo_cat_models_find(&app->models, id)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_ARGUMENT,
            "Model is not installed: %s", id ? id : "");
        return false;
    }
    if (!app->settings.model.multiple_pets)
        return active ? bongo_cat_app_select_model_with_error(app, id, error)
            : true;
    bool primary = !strcmp(app->session.active_model_id, id);
    if (active) {
        if (!bongo_cat_session_add_model(&app->session, id)) {
            bongo_cat_error_set(error, BONGO_CAT_ERROR_ARGUMENT,
                "At most %d desktop pets can be displayed",
                BONGO_CAT_ACTIVE_MODEL_CAP);
            return false;
        }
    } else if (primary) {
        if (!app->session.additional_model_count) return true;
        char replacement[BONGO_CAT_ID_CAP];
        BongoCatWindowState replacement_window;
        snprintf(replacement, sizeof(replacement), "%s",
            app->session.additional_model_ids[0]);
        bool preserve_window = load_pet_window(app, replacement,
            &replacement_window);
        if (!bongo_cat_app_select_model_with_error(app, replacement, error))
            return false;
        bongo_cat_session_remove_model(&app->session, replacement);
        if (preserve_window) adopt_pet_window(app, &replacement_window);
    } else bongo_cat_session_remove_model(&app->session, id);
    bongo_cat_multi_pet_primary_update(app, SDL_GetTicksNS());
    return true;
}

/* 关掉多桌宠: 清空副模型列表, 已在跑的子进程交给 update 的 stopping 流程收掉 (副桌宠进程内不触发 update)。
 */
void bongo_cat_app_set_multiple_pets(BongoCatApp *app, bool enabled) {
    if (!app) return;
    app->settings.model.multiple_pets = enabled;
    if (!enabled) bongo_cat_session_clear_additional_models(&app->session);
    if (!app->secondary_pet)
        bongo_cat_multi_pet_primary_update(app, SDL_GetTicksNS());
}

/* 每帧清理: 先让会话自校验 (夹 count、去重、丢弃空 id), 再丢掉已经不在模型清单里的副模型。 */
void bongo_cat_multi_pet_prune_selection(BongoCatApp *app) {
    bongo_cat_session_validate(&app->session);
    for (size_t i = app->session.additional_model_count; i > 0; --i) {
        const char *id = app->session.additional_model_ids[i - 1];
        if (!bongo_cat_models_find(&app->models, id))
            bongo_cat_session_remove_model(&app->session, id);
    }
}
