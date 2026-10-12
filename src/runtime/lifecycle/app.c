/*
 * 应用装配入口: 按固定顺序把各子系统拉起来, 顺序本身就是契约。
 *   paths-ready -> configuration-ready -> assets-ready -> window-ready
 *   -> platform-ready -> model-ready -> (第一帧真的呈现) running
 * 每一步的前置条件:
 *   - configuration-ready 之前必须已经读完配置 (settings/session), 之后的代码才允许
 *     依赖设置值; "自动提权"检查就是靠这一点才能被设置项关掉 (否则开机自启会弹 UAC)。
 *   - assets-ready 之前不能建窗口、不能建 i18n: 两者都要 asset_root / locale_root。
 *   - window-ready 之前不能碰 platform 层 (platform_init 需要 SDL_Window)。
 *   - model-ready 之前 live2d 必须已创建, 模型目录必须已扫描。
 * 失败语义: 返回 false 时只填 error, 不做任何释放 —— 调用方 (app_entry.c) 负责
 * startup_failure + app_shutdown, 而 shutdown 是按指针是否为 NULL 逐个释放的,
 * 所以这里**不要**提前 free 任何已建好的东西。
 * 副进程 (--secondary-pet=, 多宠) 跳过托盘/设置/更新, 并且初始强制不可见。
 */
#include "runtime.h"
#include "storage_paths.h"
#include "bongo_cat/audio.h"
#include "bongo_cat/file.h"
#include "bongo_cat/i18n.h"
#include "bongo_cat/path.h"
#include "bongo_cat/overlay.h"
#include "bongo_cat/preferences.h"
#include "bongo_cat/tray.h"
#include <SDL3/SDL_opengl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void scan_models(BongoCatApp *app) {
    bongo_cat_app_rescan_models(app);
}
/* 选一个能用的模型: session 里记的 id 可能已经不存在 (用户删过模型/换了机器), 于是
   依次退回 standard -> keyboard -> gamepad, 再退到列表第一个; 最后才尝试重建内置
   模型。整段的存在就是为了让"存档指向的模型没了"不变成启动失败, 而不是为了兜底
   所有错误 —— 一个都加载不起来时才填 error 返回 false。 */
static bool load_selected_model(BongoCatApp *app, BongoCatError *error) {
    const char *candidates[] = {app->session.active_model_id, "standard", "keyboard", "gamepad"};
    for (size_t i = 0; i < 4; ++i) {
        bool duplicate = false;
        for (size_t j = 0; j < i; ++j)
            if (strcmp(candidates[i], candidates[j]) == 0) duplicate = true;
        if (!duplicate && bongo_cat_models_find(&app->models, candidates[i]) &&
            bongo_cat_app_select_model(app, candidates[i])) {
            if (i) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Selected model was unavailable; loaded fallback model %s", candidates[i]);
            return true;
        }
    }
    for (size_t i = 0; i < app->models.count; ++i)
        if (bongo_cat_app_select_model(app, app->models.entries[i].id)) return true;
    /* Stored built-ins may themselves be damaged. Retry the packaged assets
       before treating a model failure as a startup failure. */
    if (bongo_cat_model_catalog_add_bundled(app, true)) {
        for (size_t i = 1; i < 4; ++i)
            if (bongo_cat_app_select_model(app, candidates[i])) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Recovered startup using bundled model %s", candidates[i]);
                return true;
            }
    }
    bongo_cat_error_set(error, BONGO_CAT_ERROR_CUBISM,
        "No usable Live2D model could be loaded");
    return false;
}
/*
 * 初始化分两段: app_entry.c 负责进程级的事 (单实例、参数), 这里负责应用装配。
 * 先 memset 再填默认值, 之后每个指针都可能是 NULL, 这是 shutdown 幂等释放的前提。
 * 顺序上 startup_prepare 必须最先 (它解析参数 + 算存储路径 + 装日志), 否则后面的
 * config_store_load 既没有路径也没有日志。
 */
bool bongo_cat_app_initialize(BongoCatApp *app, int argc, char **argv,
    BongoCatError *error) {
    memset(app, 0, sizeof(*app));
    if (argc > 0 && argv && argv[0])
        snprintf(app->executable_path, sizeof(app->executable_path),
            "%s", argv[0]);
    app->smoke_language = -1;
    app->smoke_theme = -1;
    app->smoke_preference_page = -1;
    app->applied_max_fps = -1;
    /* -1 = 命令行没有覆盖渲染后端, 按 settings.json 走 */
    app->renderer_override = -1;
    bongo_cat_settings_defaults(&app->settings);
    bongo_cat_session_defaults(&app->session);
    bongo_cat_input_init(&app->input);
    bongo_cat_shortcut_init(&app->shortcut_state);
    bongo_cat_models_init(&app->models);
    if (!bongo_cat_startup_prepare(app, argc, argv, error)) return false;
    bongo_cat_config_store_load(app);
    /* The settings cache_path is only known after the config store loads, so
       re-apply it here. A CLI override was already handled during path prep. */
    {
        BongoCatError cache_error = {0};
        if (bongo_cat_storage_paths_apply_cache(app, &cache_error) == false &&
            app->cache_root_override[0])
            return false;
        if (cache_error.message[0])
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s", cache_error.message);
    }
/* 副进程的模型与初始位置都由主进程通过命令行给: 这里覆盖 session, 并且清掉
   "多宠"以外的附加模型, 免得子进程再去读主进程的收藏列表。 */
    if (app->secondary_pet) {
        snprintf(app->session.active_model_id,
            sizeof(app->session.active_model_id), "%s",
            app->secondary_model_id);
        bongo_cat_session_clear_additional_models(&app->session);
        if (!app->session_store_valid && app->secondary_origin_known) {
            app->session.window.position_known = true;
            app->session.window.x = app->secondary_origin_x;
            app->session.window.y = app->secondary_origin_y;
        }
        /* The control file reveals the child on its first update. Starting
           hidden prevents a stale child session from flashing on screen. */
        app->session.window.visible = false;
    } else if (!app->settings.model.multiple_pets)
        bongo_cat_session_clear_additional_models(&app->session);
/* --ci-* 覆盖只影响这一次运行, 不写回配置 (config_store 在 smoke 下完全不落盘)。 */
    if (app->smoke_language >= 0)
        app->settings.app.language = (BongoCatLanguage)app->smoke_language;
    if (app->smoke_theme >= 0) app->settings.app.theme = (BongoCatTheme)app->smoke_theme;
    if (app->smoke_pass_through) app->settings.window.pass_through = true;
    if (app->smoke_model[0])
        snprintf(app->session.active_model_id, sizeof(app->session.active_model_id),
            "%s", app->smoke_model);
    if (!app->secondary_pet && !app->autostart_launch)
        app->session.window.visible = true;
    /* Keep a normally visible session hidden until the renderer has produced
       its first complete frame. The native window is still configured and
       sized below, but no platform gets a chance to expose an empty back
       buffer during model/driver startup. */
/* startup_visibility_pending 与 app_loop.c 里的 reveal_startup 配对: 窗口已经按最终
   尺寸配置好了, 但在第一帧真的端出来之前不让平台显示它, 免得露出空的后台缓冲。 */
    app->startup_visibility_pending = app->session.window.visible;
    bongo_cat_startup_stage(app, "configuration-ready");
    /*
     * 不是管理员就以管理员身份重启自己 (ShellExecuteExW 的 runas), 旧进程退出。
     * 放在这里是因为要先读配置: 设置里的"不以管理员运行"可以让它整条跳过
     * (开机自启就得靠这个开关)。用户取消 UAC 或提权失败时返回 false, 照常运行。
     */
    if (!app->settings.window.disable_elevation &&
        bongo_cat_platform_relaunch_elevated_if_needed())
        return true;
    if (bongo_cat_app_locate_assets(app, error) != BONGO_CAT_OK) return false;
    bongo_cat_startup_stage(app, "assets-ready");
    BongoCatError optional = {0};
    app->i18n = bongo_cat_i18n_create(app->locale_root, app->settings.app.language, &optional);
    if (!app->i18n) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s", optional.message);
    if (bongo_cat_window_create(app, error) != BONGO_CAT_OK) return false;
    bongo_cat_startup_stage(app, "window-ready");
    if (bongo_cat_platform_init(&app->platform, app->window, &app->input, error) != BONGO_CAT_OK) return false;
    bongo_cat_window_apply(app);
    bongo_cat_startup_stage(app, "platform-ready");
    app->live2d = bongo_cat_live2d_create(app->asset_root, error);
    if (!app->live2d) return false;
    optional = (BongoCatError){0}; app->overlay = bongo_cat_overlay_create(&optional);
    if (!app->overlay) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
        "Overlay disabled: %s", optional.message);
    optional = (BongoCatError){0}; app->audio = bongo_cat_audio_create(&optional);
    if (!app->audio) SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "%s", optional.message);
    else bongo_cat_audio_set_enabled(app->audio, true);
/* 扫描 + 选模型是 model-ready 的全部前提: 走到这里才算"有画面可画"; 这一步失败
   就整个启动失败, 因为它没有可降级的替代路径。 */
    scan_models(app);
    if (!load_selected_model(app, error)) return false;
    bongo_cat_startup_stage(app, "model-ready");
/* CI 专用: --ci-import= 会走一次真实导入 (顺带验证导入器), 失败立刻按 CI 失败退出;
   --ci-remove-imported 紧接着把它删掉, 用来验证"导入 -> 删除"这条路径。 */
    if (app->smoke_import_path[0]) {
        BongoCatError import_error = {0};
        if (bongo_cat_app_import_model(app, app->smoke_import_path, &import_error) != BONGO_CAT_OK) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", import_error.message);
            bongo_cat_startup_ci_failure(app, &import_error);
        } else if (app->smoke_remove_imported) {
            char imported[BONGO_CAT_PATH_CAP];
            snprintf(imported, sizeof(imported), "%s", app->session.active_model_id);
            if (bongo_cat_app_remove_model(app, imported, &import_error) != BONGO_CAT_OK)
                bongo_cat_startup_ci_failure(app, &import_error);
        }
    }
/* running 之后创建的托盘/设置/更新都是可选件, 任何一个起不来都只降级不失败。
   副进程不创建它们 (它没有独立入口, 全部操作由主进程经控制文件下发)。
   最后一个兜底: 既没有托盘又不可见, 用户就没有任何入口了, 强制显示。 */
    app->running = true;
    optional = (BongoCatError){0}; app->tray = app->secondary_pet ? NULL :
        bongo_cat_tray_create(app, &optional);
    if (!app->secondary_pet && !app->tray && app->settings.app.tray_visible)
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s", optional.message);
    app->preferences = app->secondary_pet ? NULL :
        bongo_cat_preferences_create(app);
    if (!app->secondary_pet && !app->preferences)
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
        "Preferences are unavailable because their state could not be allocated");
    app->update = app->secondary_pet ? NULL : bongo_cat_update_create(app);
    if (!app->secondary_pet && !app->update)
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "The update checker could not be initialized");
    bongo_cat_update_start_automatic(app->update);
    if (!app->secondary_pet && !app->tray && !app->session.window.visible) {
        bongo_cat_window_set_visible(app, true);
    }
    bongo_cat_live2d_audit_run(app);
    if (app->smoke_context_menu) bongo_cat_window_show_context_menu(app);
    if (app->smoke_shortcuts && !bongo_cat_app_shortcuts_self_test(app)) {
        BongoCatError shortcut_error = {0};
        bongo_cat_error_set(&shortcut_error, BONGO_CAT_ERROR_PLATFORM, "Shortcut action self-test failed");
        bongo_cat_startup_ci_failure(app, &shortcut_error);
    }
    if (app->smoke_menu) {
        bool menu = bongo_cat_window_menu_self_test(app);
        bool geometry = bongo_cat_window_geometry_self_test(app);
        bool display = bongo_cat_window_display_self_test(app);
        bool wheel = bongo_cat_window_wheel_self_test(app);
        bool tray = bongo_cat_tray_self_test(app->tray);
        bool wait = bongo_cat_window_wait_timeout_self_test();
        SDL_Log("Menu self-test: menu=%d geometry=%d display=%d wheel=%d tray=%d wait=%d",
            menu, geometry, display, wheel, tray, wait);
        if (!menu || !geometry || !display || !wheel || !tray || !wait) {
            BongoCatError menu_error = {0};
            bongo_cat_error_set(&menu_error, BONGO_CAT_ERROR_PLATFORM,
                "Context menu action self-test failed (menu=%d geometry=%d display=%d wheel=%d tray=%d wait=%d)",
                menu, geometry, display, wheel, tray, wait);
            bongo_cat_startup_ci_failure(app, &menu_error);
        }
    }
    if (app->smoke_preferences) bongo_cat_preferences_show(app->preferences);
/* last_frame_ns 必须在进主循环之前设好, 否则第一帧会被判定为"早就该出了"。
   startup_raise_due_ns 是首次置前的时间点: 留 250ms 让第一帧先画出来, 免得把
   还没画过的窗口提到最前面。 */
    app->last_frame_ns = SDL_GetTicksNS();
    app->dirty = true;
    app->startup_raise_due_ns = !app->secondary_pet && !app->smoke &&
        !app->autostart_launch && app->session.window.visible ?
        app->last_frame_ns + 250000000ull : 0;
    if (app->smoke_deadline_ns) app->smoke_deadline_ns += app->last_frame_ns;
    if (!app->session.window.visible) bongo_cat_startup_ready(app);
    return true;
}
