/* 右键菜单的装配与动作分发: bongo_cat_window_show_context_menu 收集标签/勾选态/回调交给
 * 平台侧弹出, 平台侧返回的 BongoCatMenuAction 在这里解释成具体副作用。
 * 菜单弹出期间主循环被模态循环挡住 (见 dial_window.c), 设置窗口的刷新只能靠结尾的
 * invalidate; 而菜单里的 hover 是"试穿", 由 window_menu_preview.c 负责回滚, 所以这里
 * 必须先用 preview_applied 判断动作是否已经落定, 否则会重复执行 (音效重复播放/重复切换)。
 * 多宠物模式的交互都是"请求 + 等主进程确认", 不要在这里直接改共享状态。 */
#include "runtime.h"
#include "window_menu.h"
#include "bongo_cat/i18n.h"
#include "bongo_cat/preferences.h"
#include "preferences_notice.h"

#include <stdio.h>
#include <string.h>

/* i18n 文案的薄封装: key 取自前端文案表 (pages./composables. 命名空间), fallback 是英文
 * 兜底。返回借用指针, 调用方不要释放。 */
static const char *tr(BongoCatApp *app, const char *key,
    const char *fallback) {
    return bongo_cat_i18n_get(app->i18n, key, fallback);
}

/* 切换模型并处理失败: 失败时按"设置窗口是否可见"选择就地提示条或系统消息框, 并记日志。
 * 返回是否成功; 成功会重建模型相关状态 (动作/表情表随模型变)。 */
static bool select_model(BongoCatApp *app, const char *id) {
    BongoCatError error = {0};
    if (bongo_cat_app_select_model_with_error(app, id, &error)) return true;
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Model switch failed: %s",
        error.message[0] ? error.message : "unknown error");
    const char *message = tr(app, "native.modelLoadFailed",
        "Unable to display this model");
    if (app->preferences && bongo_cat_preferences_visible(app->preferences))
        bongo_cat_preferences_notice_show(app, message, true);
    else SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, BONGO_CAT_NAME,
        message, app->window);
    return false;
}

/* 装配并弹出菜单: 所有数组都在栈上, 生命周期只覆盖这次调用 (平台侧同步读取, 不复制)。
 * 平台侧是阻塞的模态循环 (见 dial_window.c), 期间主循环完全停摆, 所以模型名、封面目录、
 * 标签都要在调用前取好; model_names/model_cover_directories 是借用指针, 指向 app->models。
 * 返回值要么已被 restore 落定 (只需刷新设置窗口), 要么交给下面的普通分发。 */
void bongo_cat_window_show_context_menu(BongoCatApp *app) {
    if (!app) return;
    bool dark_theme = app->settings.app.theme == BONGO_CAT_THEME_DARK ||
        (app->settings.app.theme == BONGO_CAT_THEME_AUTO &&
            SDL_GetSystemTheme() == SDL_SYSTEM_THEME_DARK);
    BongoCatWindowMenuPreview preview;
    bongo_cat_window_menu_preview_init(&preview, app);
    const char *model_names[BONGO_CAT_MODEL_CAP];
    const char *model_cover_directories[BONGO_CAT_MODEL_CAP];
    size_t current_model = app->models.count;
    /* count 是"当前模型不在列表里"的哨兵 (模型可能刚被删除), 菜单据此一个都不勾选。 */
    for (size_t i = 0; i < app->models.count; ++i) {
        model_cover_directories[i] = app->models.entries[i].adapter_directory;
        model_names[i] = bongo_cat_model_name(&app->settings,
            &app->models.entries[i]);
        if (!strcmp(app->models.entries[i].id, app->session.active_model_id))
            current_model = i;
    }
    char motion_names[BONGO_CAT_BEHAVIOR_CAP][BONGO_CAT_MENU_LABEL_CAP];
    char expression_names[BONGO_CAT_BEHAVIOR_CAP][BONGO_CAT_MENU_LABEL_CAP];
    bool motion_checked[BONGO_CAT_BEHAVIOR_CAP] = {false};
    /* 只填前 motion_count 项, 初始化是为了即使计数出错也不会读到未初始化的栈内存。 */
    size_t motion_count, expression_count, current_expression;
    bongo_cat_window_behavior_labels(app, motion_names, motion_checked,
        &motion_count, expression_names, &expression_count,
        &current_expression);
    BongoCatMenuLabels labels = {
        tr(app, "composables.useAppMenu.labels.preference", "Preferences"),
        tr(app, "composables.useAppMenu.labels.hideCat", "Hide Cat"),
        tr(app, "composables.useAppMenu.labels.passThrough", "Pass Through"),
        tr(app, "composables.useAppMenu.labels.alwaysOnTop", "Always on top"),
        tr(app, "composables.useAppMenu.labels.windowSize", "Window Size"),
        tr(app, "composables.useAppMenu.labels.opacity", "Opacity"),
        tr(app, "composables.useAppMenu.labels.model", "Model"),
        tr(app, "composables.useAppMenu.labels.addModel", "Add Model"),
        tr(app, "composables.useAppMenu.labels.quitApp", "Exit"),
        tr(app, "composables.useAppMenu.labels.wheelSizeHint", "Wheel: resize"),
        tr(app, "composables.useAppMenu.labels.wheelOpacityHint",
            "Ctrl+Wheel: opacity"),
        tr(app, "composables.useAppMenu.labels.motion", "Motions"),
        tr(app, "composables.useAppMenu.labels.expression", "Expressions"),
        model_names, motion_names, expression_names, motion_checked,
        app->models.count, current_model, motion_count, expression_count,
        current_expression, app->session.window.scale_percent,
        app->session.window.opacity_percent, app->settings.window.pass_through,
        app->settings.window.always_on_top, dark_theme,
        bongo_cat_window_menu_preview, bongo_cat_window_menu_preview_tick,
        bongo_cat_window_menu_restore, &preview,
        tr(app, "native.removeDesktopPet", "Close this desktop pet"),
        app->secondary_pet || (app->settings.model.multiple_pets &&
            app->session.additional_model_count > 0), NULL, NULL, NULL, 0,
        model_cover_directories};
    char audio_names[BONGO_CAT_BEHAVIOR_CAP][BONGO_CAT_MENU_LABEL_CAP];
    bool audio_checked[BONGO_CAT_BEHAVIOR_CAP] = {false};
    /* audio 是后补的字段, 只能在构造完 labels 之后单独填; 音频数组必须活到平台调用返回。 */
    labels.audio = tr(app, "pages.preference.model.behaviorModal.labels.audio", "Audio");
    labels.audio_names = audio_names;
    labels.audio_checked = audio_checked;
    bongo_cat_window_audio_labels(app, audio_names, audio_checked, &labels.audio_count);
    BongoCatMenuAction action = bongo_cat_platform_context_menu(
        &app->platform, &labels);
    /* 平台侧关闭菜单时会先用最终结果调 restore 落定预览; 已经落定的动作不能再走一遍普通
     * 分发, 否则动作会被执行两次 (音效重复播放)。 */
    if (bongo_cat_window_menu_preview_applied(&preview, action))
        bongo_cat_preferences_invalidate(app->preferences);
    else bongo_cat_window_menu_action(app, action);
}

/* 菜单动作分发: 注意开头那个区间判断 —— 只有缩放项 (SCALE_50..SCALE_200 这个数值区间) 不结束
 * 交互快照, 其余动作 (包括数值上排在区间之外的不透明度项) 都会先 snapshot_end。
 * 缩放与不透明度都要取消滚轮动画, 否则动画下一帧会把刚设的值拉回目标值。穿透/置顶要同时
 * 更新 settings 与平台侧, 并重算命中区域 (命中缓存不会自己失效)。
 * 多宠物 (secondary_pet) 下的设置改动是"向主进程发请求", 只有请求被接受才改本地状态;
 * 结尾统一 invalidate 设置窗口, 让它下一帧重画。 */
void bongo_cat_window_menu_action(BongoCatApp *app,
    BongoCatMenuAction action) {
    if (action < BONGO_CAT_MENU_SCALE_50 || action > BONGO_CAT_MENU_SCALE_200)
        bongo_cat_window_snapshot_end(app);
    if (action == BONGO_CAT_MENU_PREFERENCES) {
        if (app->secondary_pet)
            bongo_cat_multi_pet_request_preferences(app);
        else if (app->preferences)
            bongo_cat_preferences_show(app->preferences);
    } else if (action == BONGO_CAT_MENU_MODEL_ADD) {
        if (app->preferences)
            bongo_cat_preferences_open_model_import(app->preferences,
                app->window);
    } else if (action == BONGO_CAT_MENU_HIDE)
        bongo_cat_window_set_visible(app, false);
    else if (action == BONGO_CAT_MENU_PASS_THROUGH) {
        bool enabled = !app->settings.window.pass_through;
        if (!app->secondary_pet ||
            bongo_cat_multi_pet_request_pass_through(app, enabled)) {
            app->settings.window.pass_through = enabled;
            bongo_cat_window_mark_hit_dirty(app);
            bongo_cat_window_sync_click_through(app);
        }
    } else if (action == BONGO_CAT_MENU_ALWAYS_ON_TOP) {
        app->settings.window.always_on_top = !app->settings.window.always_on_top;
        bongo_cat_platform_set_always_on_top(&app->platform,
            app->settings.window.always_on_top);
        bongo_cat_window_mark_hit_dirty(app);
        bongo_cat_window_sync_click_through(app);
    } else if (action >= BONGO_CAT_MENU_SCALE_50 &&
        action <= BONGO_CAT_MENU_SCALE_200) {
        bongo_cat_window_cancel_wheel_animation(app);
        bongo_cat_window_set_scale(app,
            (float)(50 + 10 * (action - BONGO_CAT_MENU_SCALE_50)));
    } else if (action >= BONGO_CAT_MENU_OPACITY_10 &&
        action <= BONGO_CAT_MENU_OPACITY_100) {
        bongo_cat_window_cancel_wheel_animation(app);
        app->session.window.opacity_percent =
            (float)(10 * (action - BONGO_CAT_MENU_OPACITY_10 + 1));
        if (!app->hover_hidden) {
            bongo_cat_app_cancel_hover_fade(app);
            bongo_cat_platform_set_opacity(&app->platform,
                app->session.window.opacity_percent / 100.0f);
        }
    } else if (bongo_cat_window_behavior_action(app, action)) {
        bongo_cat_app_render_now(app);
    } else if (action >= BONGO_CAT_MENU_MODEL_FIRST &&
        action < BONGO_CAT_MENU_MODEL_FIRST + BONGO_CAT_MODEL_CAP) {
        size_t index = (size_t)(action - BONGO_CAT_MENU_MODEL_FIRST);
        if (!app->secondary_pet && index < app->models.count)
            select_model(app, app->models.entries[index].id);
    } else if (action == BONGO_CAT_MENU_REMOVE_PET) {
        if (app->secondary_pet) {
            if (bongo_cat_multi_pet_request_remove(app)) {
                bongo_cat_platform_set_visible(&app->platform, false);
                app->running = false;
            }
        } else if (app->settings.model.multiple_pets &&
            app->session.additional_model_count) {
            char model_id[BONGO_CAT_ID_CAP];
            BongoCatError error = {0};
            snprintf(model_id, sizeof(model_id), "%s",
                app->session.active_model_id);
            bool visible = app->session.window.visible;
            if (visible) bongo_cat_platform_set_visible(&app->platform, false);
            if (!bongo_cat_app_set_model_active(app, model_id, false, &error))
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "[runtime] Multi-pet primary remove failed: model=%s "
                    "error=%s", model_id,
                    error.message[0] ? error.message : "unknown error");
            if (visible) bongo_cat_platform_set_visible(&app->platform, true);
        }
    } else if (action == BONGO_CAT_MENU_EXIT) {
        if (app->secondary_pet) {
            if (bongo_cat_multi_pet_request_exit(app)) {
                bongo_cat_platform_set_visible(&app->platform, false);
                app->running = false;
            }
        } else app->running = false;
    }
    bongo_cat_preferences_invalidate(app->preferences);
}

/* 菜单自测: 先把四项会被检查的状态重置成已知值, 再逐个走真实分发路径并断言结果 ——
 * 副作用是真的 (会改设置、缩放、不透明度并打开设置窗口), 结束前必须关掉设置窗口。
 * 返回 false 会经由 bongo_cat_window_menu_self_test 汇成 smoke 的 CI 失败。 */
bool bongo_cat_window_menu_self_test(BongoCatApp *app) {
    if (!app || !app->preferences) return false;
    app->settings.window.pass_through = false;
    app->settings.window.always_on_top = false;
    app->session.window.scale_percent = 100.0f;
    app->session.window.opacity_percent = 100.0f;
    bongo_cat_window_menu_action(app, BONGO_CAT_MENU_PASS_THROUGH);
    bongo_cat_window_menu_action(app, BONGO_CAT_MENU_ALWAYS_ON_TOP);
    bongo_cat_window_menu_action(app, BONGO_CAT_MENU_SCALE_120);
    bongo_cat_window_menu_action(app, BONGO_CAT_MENU_OPACITY_50);
    bongo_cat_window_menu_action(app, BONGO_CAT_MENU_PREFERENCES);
    bool behavior = bongo_cat_window_behavior_self_test(app);
    bool result = app->settings.window.pass_through &&
        app->settings.window.always_on_top &&
        app->session.window.scale_percent == 120.0f &&
        app->session.window.opacity_percent == 50.0f &&
        bongo_cat_preferences_visible(app->preferences) && behavior;
    bongo_cat_preferences_close(app->preferences);
    return result;
}
