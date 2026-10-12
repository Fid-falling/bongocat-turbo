/*
 * 托盘图标与托盘菜单 (Windows 通知区域 / 各平台原生托盘)。
 *
 * 菜单文案全部来自 i18n 表 (composables.useAppMenu.labels.*), 第二个参数是查不到 key 时
 * 的英文兜底 —— 所以**新增菜单项必须同时补 i18n key**, 不能只写死英文, 否则除英文外的
 * 语言会漏翻; 切语言后靠 tray_sync 里的 last_language 比较把文案重新贴一遍。
 *
 * 托盘是可选件: 设置里关掉、加载图标失败、原生托盘创建失败都只降级为"没有托盘"
 * (见 app.c 的警告), 因此这里所有对外函数都必须容忍 NULL。
 *
 * Windows 资源管理器重启会把托盘图标一起干掉: 平台层回调 bongo_cat_tray_on_restore 只置
 * restore_requested, 由 tray_sync 销毁并重建整个托盘 (旧菜单项指针已全部失效, 不能复用),
 * 重建失败 1s 后重试。
 *
 * 每个菜单回调都要做齐三件事: 改 app 状态 -> 同步窗口 (可见性/穿透/置顶) -> tray_sync
 * (勾选态) -> preferences_invalidate (设置窗口正开着时跟着刷新)。少做一步就会出现
 * "界面显示的和实际状态不一致"。
 *
 * 本文件只管托盘的建/销毁/同步; "切换渲染后端"那一项在 tray_renderer.c, --ci-menu 的
 * 自测在 tray_self_test.c (拆分理由见 tray_internal.h)。
 */
#include "runtime.h"
#include "tray_internal.h"
#include "ui_native_theme.h"
#include "bongo_cat/i18n.h"
#include "bongo_cat/log.h"
#include "bongo_cat/path.h"
#include "bongo_cat/preferences.h"
#include "bongo_cat/render.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

/* 菜单动作。托盘菜单本身不存状态: 真相始终在 app->session / app->settings 里, 回调只负责
   改它, 再由 tray_sync 把勾选态贴回去。这几个不是 static —— tray_self_test.c 要真实
   调用它们 (见 tray_internal.h)。 */
void bongo_cat_tray_on_visible(void *userdata, SDL_TrayEntry *entry) {
    (void)entry;
    BongoCatTray *tray = userdata;
    BongoCatApp *app = tray->app;
    bongo_cat_window_set_visible(app, !app->session.window.visible);
    bongo_cat_tray_sync(tray);
    bongo_cat_preferences_invalidate(app->preferences);
}

void bongo_cat_tray_on_pass_through(void *userdata, SDL_TrayEntry *entry) {
    (void)entry;
    BongoCatTray *tray = userdata;
    tray->app->settings.window.pass_through = !tray->app->settings.window.pass_through;
    bongo_cat_window_mark_hit_dirty(tray->app);
    bongo_cat_window_sync_click_through(tray->app);
    bongo_cat_tray_sync(tray);
    bongo_cat_preferences_invalidate(tray->app->preferences);
}

void bongo_cat_tray_on_always_on_top(void *userdata, SDL_TrayEntry *entry) {
    (void)entry;
    BongoCatTray *tray = userdata;
    tray->app->settings.window.always_on_top = !tray->app->settings.window.always_on_top;
    bongo_cat_platform_set_always_on_top(&tray->app->platform,
        tray->app->settings.window.always_on_top);
    bongo_cat_window_mark_hit_dirty(tray->app);
    bongo_cat_window_sync_click_through(tray->app);
    bongo_cat_tray_sync(tray);
    bongo_cat_preferences_invalidate(tray->app->preferences);
}

void bongo_cat_tray_on_preferences(void *userdata, SDL_TrayEntry *entry) {
    (void)entry;
    BongoCatTray *tray = userdata;
    bongo_cat_preferences_show(tray->app->preferences);
}

static void on_tray_left_click(void *userdata) {
    BongoCatTray *tray = userdata;
    if (tray && tray->app && tray->app->preferences)
        bongo_cat_preferences_show(tray->app->preferences);
}

static void on_tray_modal_frame(void *userdata) {
    BongoCatTray *tray = userdata;
    if (tray) bongo_cat_modal_frame_tick(&tray->modal_frame);
}

void bongo_cat_tray_on_exit(void *userdata, SDL_TrayEntry *entry) {
    (void)entry;
    ((BongoCatTray *)userdata)->app->running = false;
}

void bongo_cat_tray_on_restore(void *userdata) {
    BongoCatTray *tray = userdata;
    if (!tray) return;
    if (!tray->restore_requested)
        SDL_Log("Windows shell restarted; scheduling tray restoration");
    tray->restore_requested = true;
    tray->restore_after_ns = 0;
}

/* 原生菜单的深浅色由平台层统一处理, 这里只把系统主题传下去。 */
void bongo_cat_tray_prepare_menu(BongoCatTray *tray, void *native_handle) {
    if (!tray || !tray->app) return;
    bool dark = SDL_GetSystemTheme() == SDL_SYSTEM_THEME_DARK;
    bongo_cat_ui_native_menu_prepare_native(native_handle, dark);
}

/* 插入菜单项的统一封装: 一律追加到末尾 (-1 是 SDL 的"追加"约定), 并把回调与 userdata
   一起绑上。label 为 NULL 就是分隔线, 不绑回调。 */
static SDL_TrayEntry *add(SDL_TrayMenu *menu, const char *label,
    SDL_TrayEntryFlags flags, SDL_TrayCallback callback, BongoCatTray *tray) {
    SDL_TrayEntry *entry = SDL_InsertTrayEntryAt(menu, -1, label, flags);
    if (entry && callback) SDL_SetTrayEntryCallback(entry, callback, tray);
    return entry;
}
/* 先摘掉平台层回调再销毁: 否则销毁过程中到达的回调会拿到已经释放的 tray。 */
static void destroy_native_tray(BongoCatTray *tray) {
    if (!tray || !tray->handle) return;
    bongo_cat_platform_set_tray_callbacks(
        tray->handle, NULL, NULL, NULL, NULL);
    SDL_DestroyTray(tray->handle);
    tray->handle = NULL;
    tray->visible = tray->pass_through = tray->always_on_top = NULL;
    tray->preferences = tray->renderer = tray->exit = NULL;
    tray->state_valid = false;
}

/* 菜单项是顺序敏感的 (两条分隔线的位置固定), 所以任何一项建失败都整体销毁重来 ——
   半截菜单比没有菜单更难查。 */
static bool create_native_tray(BongoCatTray *tray, BongoCatError *error) {
    tray->handle = SDL_CreateTray(tray->icon.surface, BONGO_CAT_TRAY_TITLE);
    if (!tray->handle) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Tray creation failed: %s", SDL_GetError());
        return false;
    }
    SDL_TrayMenu *menu = SDL_CreateTrayMenu(tray->handle);
    SDL_TrayEntry *separator1 = NULL, *separator2 = NULL;
    if (menu) {
        tray->preferences = add(menu, bongo_cat_i18n_get(tray->app->i18n,
            "composables.useAppMenu.labels.preference", "Preferences"),
            SDL_TRAYENTRY_BUTTON, bongo_cat_tray_on_preferences, tray);
        /* 紧跟在"设置"后面: 设置窗口打不开/画不出来时, 它是紧接着要试的那一项。 */
        tray->renderer = add(menu, bongo_cat_tray_renderer_label(tray),
            SDL_TRAYENTRY_BUTTON, bongo_cat_tray_on_renderer, tray);
        separator1 = add(menu, NULL, 0, NULL, tray);
        tray->visible = add(menu, bongo_cat_i18n_get(tray->app->i18n,
            "composables.useAppMenu.labels.showCat", "Show BongoCat"),
            SDL_TRAYENTRY_CHECKBOX, bongo_cat_tray_on_visible, tray);
        tray->pass_through = add(menu, bongo_cat_i18n_get(tray->app->i18n,
            "composables.useAppMenu.labels.passThrough", "Mouse pass-through"),
            SDL_TRAYENTRY_CHECKBOX, bongo_cat_tray_on_pass_through, tray);
        tray->always_on_top = add(menu, bongo_cat_i18n_get(tray->app->i18n,
            "composables.useAppMenu.labels.alwaysOnTop", "Always on top"),
            SDL_TRAYENTRY_CHECKBOX, bongo_cat_tray_on_always_on_top, tray);
        separator2 = add(menu, NULL, 0, NULL, tray);
        tray->exit = add(menu, bongo_cat_i18n_get(tray->app->i18n,
            "composables.useAppMenu.labels.quitApp", "Exit"),
            SDL_TRAYENTRY_BUTTON, bongo_cat_tray_on_exit, tray);
    }
    if (!menu || !tray->preferences || !tray->renderer || !separator1 ||
        !tray->visible || !tray->pass_through || !tray->always_on_top ||
        !separator2 || !tray->exit) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Tray menu creation failed: %s", SDL_GetError());
        destroy_native_tray(tray);
        return false;
    }
    bongo_cat_platform_set_tray_callbacks(tray->handle,
        on_tray_left_click, on_tray_modal_frame, bongo_cat_tray_on_restore, tray);
    tray->state_valid = false;
    return true;
}

/* app->settings.app.tray_visible 为 false (用户在设置里关掉托盘) 时返回 NULL 不算错误;
   图标加载或原生托盘创建失败才算失败, 调用方降级为"没有托盘"。 */
BongoCatTray *bongo_cat_tray_create(BongoCatApp *app, BongoCatError *error) {
    if (!app || !app->settings.app.tray_visible) return NULL;
    BongoCatTray *tray = calloc(1, sizeof(*tray));
    if (!tray) return NULL;
    tray->app = app;
    bongo_cat_modal_frame_init(&tray->modal_frame, app);
    char path[BONGO_CAT_PATH_CAP];
    bongo_cat_path_join(path, sizeof(path), app->asset_root, "bongocat.png");
    if (bongo_cat_image_load(path, &tray->icon, error) != BONGO_CAT_OK) {
        bongo_cat_tray_destroy(tray);
        return NULL;
    }
    if (!create_native_tray(tray, error)) {
        bongo_cat_tray_destroy(tray);
        return NULL;
    }
    SDL_Log("Tray created");
    bongo_cat_tray_sync(tray);
    return tray;
}

/* restore_requested 表示"Windows shell 重启过": 先销毁再重建, 失败时保留标志并 1s 后重试
   (旧菜单项指针全部失效, 不能复用)。 */
static bool restore_native_tray(BongoCatTray *tray) {
    if (!tray->restore_requested) return tray->handle != NULL;
    uint64_t now = SDL_GetTicksNS();
    if (now < tray->restore_after_ns) return tray->handle != NULL;
    destroy_native_tray(tray);
    BongoCatError error = {0};
    if (!create_native_tray(tray, &error)) {
        tray->restore_requested = true;
        tray->restore_after_ns = now + 1000000000ull;
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Tray restoration failed; retrying: %s", error.message);
        return false;
    }
    tray->restore_requested = false;
    tray->restore_count++;
    SDL_Log("Tray restored after Windows shell restart (count=%u)",
        tray->restore_count);
    return true;
}

/* 幂等 + 便宜的脏检查: 只有五项状态 (可见/穿透/置顶/语言/渲染后端) 之一变了才真的改
   菜单, 因为 SDL_UpdateTrays 会让 shell 重画托盘。语言也算一项 —— 换语言必须重贴文案;
   渲染后端也算 —— 那一项的文案里写着"当前 -> 目标", 不重贴就会一直显示旧状态。 */
void bongo_cat_tray_sync(BongoCatTray *tray) {
    if (!tray || !restore_native_tray(tray)) return;
    BongoCatApp *app = tray->app;
    const int renderer = (int)bongo_cat_renderer_active();
    if (tray->state_valid && tray->last_visible == app->session.window.visible &&
        tray->last_pass_through == app->settings.window.pass_through &&
        tray->last_always_on_top == app->settings.window.always_on_top &&
        tray->last_language == app->settings.app.language &&
        tray->last_renderer == renderer) return;
    tray->state_valid = true;
    tray->last_visible = app->session.window.visible;
    tray->last_pass_through = app->settings.window.pass_through;
    tray->last_always_on_top = app->settings.window.always_on_top;
    tray->last_language = app->settings.app.language;
    tray->last_renderer = renderer;
    SDL_SetTrayEntryChecked(tray->visible, tray->app->session.window.visible);
    SDL_SetTrayEntryChecked(tray->pass_through, tray->app->settings.window.pass_through);
    SDL_SetTrayEntryChecked(tray->always_on_top, tray->app->settings.window.always_on_top);
    // The checkbox already communicates visibility; keep the action name
    // stable so users do not have to reinterpret the menu after each click.
    SDL_SetTrayEntryLabel(tray->visible, bongo_cat_i18n_get(tray->app->i18n,
        "composables.useAppMenu.labels.showCat", "Show BongoCat"));
    SDL_SetTrayEntryLabel(tray->pass_through, bongo_cat_i18n_get(tray->app->i18n,
        "composables.useAppMenu.labels.passThrough", "Mouse pass-through"));
    SDL_SetTrayEntryLabel(tray->always_on_top, bongo_cat_i18n_get(tray->app->i18n,
        "composables.useAppMenu.labels.alwaysOnTop", "Always on top"));
    SDL_SetTrayEntryLabel(tray->preferences, bongo_cat_i18n_get(tray->app->i18n,
        "composables.useAppMenu.labels.preference", "Preferences"));
    SDL_SetTrayEntryLabel(tray->renderer, bongo_cat_tray_renderer_label(tray));
    SDL_SetTrayEntryLabel(tray->exit, bongo_cat_i18n_get(tray->app->i18n,
        "composables.useAppMenu.labels.quitApp", "Exit"));
    SDL_UpdateTrays();
}

/* --ci-menu 用的自测: 走一遍真实回调并每项点两次 (验证改回去也能恢复), 最后检查状态与
   进入时一致; Windows 上还会先模拟一次 shell 重启。 */

void bongo_cat_tray_destroy(BongoCatTray *tray) {
    if (!tray) return;
    destroy_native_tray(tray);
    bongo_cat_image_free(&tray->icon);
    free(tray);
}
