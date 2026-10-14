/*
 * 设置页 [应用] 区 + macOS 的输入监控授权区 (从 preferences_pages.c 拆出来)。
 *
 * 为什么单独一个文件: preferences_pages.c 在并入拖动惯性那一版之后超过了 500 行的策略
 * 上限 (见 cmake/CheckLines.cmake), 而"应用/外观"这一页从开关副作用到提权重启都自成
 * 一块 —— 与 [窗口] 区没有共享状态, 正好搬出来。
 *
 * 两条容易踩空的地方, 改之前先看:
 *   1. "以管理员身份运行"的开关不是纯写配置: 开 + 普通权限 = 立刻提权重启。冒烟
 *      (--ci-*) 与副宠进程里禁止提权/重启, 必须原值弹回, 见 update_run_as_admin。
 *   2. 开机自启的目标状态是 [快捷方式 IFF autostart] + [计划任务 IFF autostart &&
 *      run_as_admin], 切换失败时要把界面上的开关恢复成 old_value, 否则用户看到的是
 *      "明明失败了却显示已开启"。
 */
#include "preferences_internal.h"
#include "preferences_notice.h"
#include "preferences_theme.h"
#include "preferences_widgets.h"
#include "bongo_cat/i18n.h"
#include "bongo_cat/log.h"
#include "bongo_cat/platform.h"
#include "runtime.h"

#include <SDL3/SDL.h>

static const char *tr(BongoCatApp *app, const char *key, const char *fallback) {
    return bongo_cat_i18n_get(app->i18n, key, fallback);
}

static void section_gap(struct nk_context *context, float pixels) {
    context->current->layout->at_y += pixels;
}


static void update_autostart(BongoCatApp *app, bool old_value) {
    BongoCatError error = {0};
    if (bongo_cat_platform_set_autostart(app->settings.app.autostart,
            app->settings.app.run_as_admin, &error) == BONGO_CAT_OK) return;
    app->settings.app.autostart = old_value;
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", error.message);
    bongo_cat_preferences_notice_show(app, tr(app,
        "pages.preference.general.hints.autostartFailed",
        "Unable to update launch-on-startup settings"), true);
}

/*
 * "以管理员身份运行" (run_as_admin) 切换后的处理:
 *   - 冒烟/副宠是被自动化或主进程拉起来的, 既不能提权也不能重启, 直接拒绝并恢复原值;
 *   - 开启且当前不是管理员: 立刻以管理员身份重启 (新值已由 config_store 的延迟保存 +
 *     重启前的 flush 落盘), 旧进程退出;
 *   - 开启且已经是管理员 / 关闭: 无需重启, 只提示。
 * 设置本身走 app->dirty 的延迟落盘, 和本页其它开关一致。
 */
static void update_run_as_admin(BongoCatApp *app) {
    if (app->smoke || app->secondary_pet) {
        app->settings.app.run_as_admin = !app->settings.app.run_as_admin;
        app->dirty = true;
        bongo_cat_preferences_notice_show(app, tr(app,
            "pages.preference.general.hints.runAsAdminUnavailable",
            "Changing administrator settings is unavailable in this session."),
            true);
        return;
    }
    app->dirty = true;
    if (!app->settings.app.run_as_admin) {
        /* 关掉: 只影响下一次启动, 正在运行的进程不必动。若开着开机自启, 要同时把自启
           从最高权限计划任务降回普通快捷方式(提权会话内是静默的)。 */
        if (app->settings.app.autostart) {
            BongoCatError sync_error = {0};
            if (bongo_cat_platform_set_autostart(true, false, &sync_error) != BONGO_CAT_OK)
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "[autostart] 降级自启失败: %s", sync_error.message);
        }
        bongo_cat_preferences_notice_show(app, tr(app,
            "pages.preference.general.hints.runAsAdminOff",
            "Settings saved. BongoCat will run without administrator next time."),
            false);
        return;
    }
    /* 开 + 当前已是管理员: 已经在提权状态, 无需再重启; 若开着自启, 顺手把自启升级成
       计划任务(静默, 下次开机免 UAC)。 */
    if (bongo_cat_platform_is_elevated()) {
        if (app->settings.app.autostart) {
            BongoCatError sync_error = {0};
            if (bongo_cat_platform_set_autostart(true, true, &sync_error) != BONGO_CAT_OK)
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "[autostart] 升级自启失败: %s", sync_error.message);
        }
        bongo_cat_preferences_notice_show(app, tr(app,
            "pages.preference.general.hints.runAsAdminAlready",
            "BongoCat is already running as administrator."), false);
        return;
    }
    /* 开 + 普通权限: 立即以管理员身份重启。被无人值守 (--ci-* / 冒烟) 挡下时提示手动重启。 */
    if (bongo_cat_app_request_restart_elevated(app, true)) {
        bongo_cat_preferences_notice_show(app, tr(app,
            "pages.preference.cat.hints.rendererRestarting",
            "Restarting BongoCat to apply the new render backend..."), false);
    } else {
        bongo_cat_preferences_notice_show(app, tr(app,
            "pages.preference.general.hints.runAsAdminRestart",
            "Settings saved. Restart BongoCat manually as administrator to apply it."),
            false);
    }
}

#ifdef __APPLE__
/* System Settings keeps Input Monitoring under Privacy & Security. */
#define BONGO_CAT_INPUT_MONITORING_SETTINGS_URI \
    "x-apple.systempreferences:com.apple.preference.security?Privacy_ListenEvent"

void bongo_cat_preferences_input_monitoring_refresh(BongoCatPreferences *value) {
    if (!value) return;
    /* The refresh points are user actions, not frames: one read here. */
    bool authorized = bongo_cat_platform_input_monitoring_authorized();
    /* A button drawn from the old reading stays on screen until something
       repaints, so a reading that changed - or arrives for the first time -
       schedules the next frame; an unchanged one must not, or every refresh
       point would keep the window rendering. */
    if (value->input_monitoring_valid &&
        value->input_monitoring_authorized == authorized) return;
    value->input_monitoring_authorized = authorized;
    value->input_monitoring_valid = true;
    value->render_dirty = true;
}

static void enable_input_monitoring(BongoCatApp *app) {
    BongoCatPreferences *value = app->preferences;
    if (!value) return;
    /* The permission can change while the page is open, so the button reads it
       once more before acting: an already granted permission hides the button
       instead of asking again or sending the user to System Settings. */
    bongo_cat_preferences_input_monitoring_refresh(value);
    if (value->input_monitoring_authorized) return;
    /* The system prompt belongs to this button and to a missing permission; a
       refusal is an answer, not a failure. */
    (void)bongo_cat_platform_input_monitoring_request();
    /* SDL_OpenURL is the whole entry point, so a failure is where the path has
       to be spelled out: the item itself only says when to restart. */
    if (!SDL_OpenURL(BONGO_CAT_INPUT_MONITORING_SETTINGS_URI))
        bongo_cat_preferences_notice_show(app, tr(app,
            "pages.preference.general.status.openSettingsFailed",
            "Cannot open System Settings. Open Privacy & Security → Input "
            "Monitoring."), true);
    bongo_cat_preferences_input_monitoring_refresh(value);
}

static void input_monitoring_section(BongoCatApp *app,
    struct nk_context *context) {
    /* The page is drawn through the preferences that hold the cached reading; a
       page drawn before the first refresh initializes it once, never per frame. */
    BongoCatPreferences *value = app->preferences;
    if (!value) return;
    if (!value->input_monitoring_valid)
        bongo_cat_preferences_input_monitoring_refresh(value);
    bongo_cat_pref_section_icon(context, tr(app,
        "pages.preference.general.labels.permissionsSettings",
        "Permissions Settings"), BONGO_CAT_PREF_ICON_SECTION_APPLICATION);
    const char *title = tr(app,
        "pages.preference.general.labels.inputMonitoringPermission",
        "Input Monitoring Permission");
    /* Granted is a reading without an action; only a missing permission offers
       the button, and nothing here asks for authorization twice. */
    if (value->input_monitoring_authorized) {
        bongo_cat_pref_status(context, "input-monitoring", title, tr(app,
            "pages.preference.general.status.authorized", "Authorized"));
        return;
    }
    if (bongo_cat_pref_button(context, "input-monitoring", title, tr(app,
        "pages.preference.general.hints.inputMonitoringPermission",
        "Restart after granting it."), tr(app,
        "pages.preference.general.status.authorize", "Go to Enable")))
        enable_input_monitoring(app);
}
#endif

void bongo_cat_preferences_general_page(BongoCatApp *app,
    struct nk_context *context) {
    BongoCatApplicationPreferences *options = &app->settings.app;
    // Keep each option in its own native language so the list is recognizable
    // regardless of the language currently used by the settings window.
    const char *ui_languages[] = {"简体中文", "繁體中文", "English",
        "Français", "Deutsch", "日本語", "한국어", "Português",
        "Русский", "Español"};
    bongo_cat_pref_section_icon(context, tr(app,
        "pages.preference.general.labels.appSettings", "Application"),
        BONGO_CAT_PREF_ICON_SECTION_APPLICATION);
    bool old_autostart = options->autostart;
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_AUTOSTART);
    if (bongo_cat_pref_toggle(context, "autostart", tr(app,
        "pages.preference.general.labels.launchOnStartup", "Launch on Startup"), "",
        &options->autostart)) update_autostart(app, old_autostart);
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_RENDERER);
    if (bongo_cat_pref_toggle(context, "run-as-admin", tr(app,
        "pages.preference.general.labels.runAsAdmin", "Run as Administrator"),
        tr(app, "pages.preference.general.labels.runAsAdminHint",
            "Requests administrator rights at startup. While enabled, a UAC prompt appears every launch, including auto-start."),
        &options->run_as_admin)) update_run_as_admin(app);
    section_gap(context, 7);
    bongo_cat_pref_section_icon(context, tr(app,
        "pages.preference.general.labels.appearanceSettings", "Appearance"),
        BONGO_CAT_PREF_ICON_SECTION_APPEARANCE);
    section_gap(context, 6);
    const int language_to_ui[] = {2, 0, 1, 3, 4, 5, 6, 7, 8, 9};
    const BongoCatLanguage ui_to_language[] = {
        BONGO_CAT_LANG_ZH_CN, BONGO_CAT_LANG_ZH_HANT,
        BONGO_CAT_LANG_EN_US, BONGO_CAT_LANG_FR_FR,
        BONGO_CAT_LANG_DE_DE, BONGO_CAT_LANG_JA_JP,
        BONGO_CAT_LANG_KO_KR, BONGO_CAT_LANG_PT_BR,
        BONGO_CAT_LANG_RU_RU, BONGO_CAT_LANG_ES_ES};
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_LANGUAGE);
    int selected = bongo_cat_pref_combo(context,
        "language", tr(app, "pages.preference.general.labels.language",
        "Language"), "", ui_languages, BONGO_CAT_LANG_COUNT,
        language_to_ui[options->language]);
    options->language = ui_to_language[selected];
    const char *themes[] = {
        tr(app, "pages.preference.general.options.auto", "System"),
        tr(app, "pages.preference.general.options.lightMode", "Light"),
        tr(app, "pages.preference.general.options.darkMode", "Dark")};
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_THEME);
    options->theme = (BongoCatTheme)bongo_cat_pref_theme(context,
        "theme", tr(app, "pages.preference.general.labels.themeMode",
        "Theme"), themes, options->theme);
#ifdef __APPLE__
    section_gap(context, 7);
    input_monitoring_section(app, context);
#endif
}
