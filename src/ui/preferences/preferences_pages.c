#include "preferences_internal.h"
#include "preferences_state.h"
#include "preferences_theme.h"
#include "preferences_widgets.h"
#include "preferences_notice.h"
#include "ui_tooltip.h"
#include "bongo_cat/audio.h"
#include "bongo_cat/i18n.h"
#include "bongo_cat/log.h"
#include "bongo_cat/preferences.h"
#include "bongo_cat/render.h"
#include "bongo_cat/tray.h"
#include "runtime.h"

#include <SDL3/SDL.h>
#include <string.h>

static const char *tr(BongoCatApp *app, const char *key, const char *fallback) {
    return bongo_cat_i18n_get(app->i18n, key, fallback);
}

static void section_gap(struct nk_context *context, float pixels) {
    context->current->layout->at_y += pixels;
}

static void page_display(BongoCatPreferences *value, struct nk_context *context) {
    BongoCatApp *app = value->app;
    BongoCatModelPreferences *model = &app->settings.model;
    BongoCatWindowPreferences *window = &app->settings.window;
    BongoCatWindowState *window_state = &app->session.window;
    if (bongo_cat_preferences_options_begin(value, context,
        BONGO_CAT_OPTIONS_WINDOW,
        tr(app, "pages.preference.cat.labels.windowSettings", "Window"),
        BONGO_CAT_PREF_ICON_SECTION_WINDOW)) {
    bongo_cat_ui_question_tooltip(context, tr(app,
        "pages.preference.cat.hints.gameInput", "Pet not responding in games?"),
        tr(app, "pages.preference.cat.hints.gameInputHelp",
            "Try running BongoCat as administrator and setting the game to windowed mode."));
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_PASS_THROUGH);
    if (bongo_cat_pref_toggle(context, "pass-through", tr(app,
        "composables.useAppMenu.labels.passThrough", "Pass Through"), tr(app,
        "pages.preference.cat.hints.passThrough",
        "You can also turn this off by right-clicking the tray icon"),
        &window->pass_through)) {
        bongo_cat_window_mark_hit_dirty(app);
        bongo_cat_window_sync_click_through(app);
    }
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_ALWAYS_ON_TOP);
    if (bongo_cat_pref_toggle(context, "always-top", tr(app,
        "composables.useAppMenu.labels.alwaysOnTop", "Always on Top"), "",
        &window->always_on_top)) {
        bongo_cat_platform_set_always_on_top(&app->platform, window->always_on_top);
        bongo_cat_window_mark_hit_dirty(app);
        bongo_cat_window_sync_click_through(app);
    }
    if (window->pass_through && window->always_on_top &&
        app->platform.hover_hide_unavailable) {
        bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_SHORTCUT_VISIBILITY);
        bongo_cat_pref_status(context, "hover-unavailable", tr(app,
            "pages.preference.cat.labels.hideOnHover", "Hide on Hover"), tr(app,
            "pages.preference.cat.hints.hoverUnavailable",
            "Hover hiding is unavailable in the current desktop environment."));
    }
    if (window->pass_through && window->always_on_top &&
        !app->platform.hover_hide_unavailable) {
        bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_SHORTCUT_VISIBILITY);
        if (bongo_cat_pref_toggle(context, "hide-on-hover", tr(app,
            "pages.preference.cat.labels.hideOnHover", "Hide on Hover"), "",
            &window->hide_on_hover))
            bongo_cat_app_update_hover(app, SDL_GetTicksNS());
        bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_HIDE_FADE);
        bongo_cat_pref_float(context, "hide-fade", tr(app,
            "pages.preference.cat.labels.hideFadeSeconds", "Fade Duration (s)"),
            "", 0.0f, &window->hide_fade_seconds,
            BONGO_CAT_MAX_HIDE_FADE_SECONDS, 0.1f,
            BONGO_CAT_DEFAULT_HIDE_FADE_SECONDS);
    }
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_KEEP_IN_SCREEN);
    if (bongo_cat_pref_toggle(context, "keep-in-screen", tr(app,
        "pages.preference.cat.labels.keepInScreen", "Keep on Screen"), "",
        &window->keep_in_screen) && window->keep_in_screen)
        bongo_cat_window_clamp_to_display(app);
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_FRAME_BOUNDS);
    {
        /* 吸附阈值 0-64px, 以 float 传给滑杆, 写回时取整存 int。 */
        float snap_px = (float)window->edge_snap_threshold;
        if (bongo_cat_pref_toggle_slider(context, "edge-snap", tr(app,
            "pages.preference.cat.labels.edgeSnap", "Edge Snapping"),
            tr(app, "pages.preference.cat.hints.edgeSnap",
                "Snap to a screen edge when dragging near it."),
            &window->edge_snap, 0.0f, &snap_px,
            BONGO_CAT_MAX_EDGE_SNAP_PX, 1.0f,
            BONGO_CAT_DEFAULT_EDGE_SNAP_PX, " px")) {
            window->edge_snap_threshold = (int)snap_px;
            app->dirty = true;
        }
    }
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_FRAME_BOUNDS);
    {
        /* 惯性: 开关 + 衰减档位滑杆 (0-99, 越大停得越快)。 */
        float slow = (float)window->slide_slowdown_percent;
        if (bongo_cat_pref_toggle_slider(context, "slide-animation", tr(app,
            "pages.preference.cat.labels.slideAnimation", "Sliding Animation"),
            tr(app, "pages.preference.cat.hints.slideAnimation",
                "Keep gliding a short distance after you release a drag."),
            &window->slide_animation, 0.0f, &slow,
            BONGO_CAT_MAX_SLIDE_SLOWDOWN_PERCENT, 1.0f,
            BONGO_CAT_DEFAULT_SLIDE_SLOWDOWN_PERCENT, "")) {
            window->slide_slowdown_percent = (int)slow;
            app->dirty = true;
        }
    }
    /* 只在录屏/直播软件里显示: 仅 Windows 有等价机制 (DWM 隐藏), 其它平台不显示
       这一项, 免得给用户一个点了没反应的开关。 */
    if (bongo_cat_platform_capture_only_supported()) {
        bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_SOLID_BACKGROUND);
        if (bongo_cat_pref_toggle(context, "capture-only", tr(app,
            "pages.preference.cat.labels.captureOnly", "Capture Only"), tr(app,
            "pages.preference.cat.hints.captureOnly", "Hidden on the desktop, "
            "still visible in OBS"), &window->capture_only)) {
            bongo_cat_window_apply_capture_only(app);
            app->dirty = true;
        }
    }
    /* 透明背景: 实现在 preferences_window_transparency.c, 只有 Windows 有等价机制,
       其它平台连定义都没有, 所以调用点也要跟着条件编译。 */
#ifdef _WIN32
    bongo_cat_preferences_window_transparency(app, context);
#endif
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_SOLID_BACKGROUND);
    if (bongo_cat_pref_obs_background(context, "obs-background", tr(app,
        "pages.preference.cat.labels.obsBackground", "Solid Background"), tr(app,
        "pages.preference.cat.hints.obsBackground", "Window capture is black?"), tr(app,
        "pages.preference.cat.hints.obsBackgroundHelp", "OBS: enable this option, use "
        "the Windows 7 compatibility method, and remove the background with a "
        "color key filter."),
        &window->obs_background, &window->obs_background_color))
        app->dirty = true;
    float old_scale = window_state->scale_percent;
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_WINDOW_SIZE);
    bongo_cat_pref_float(context, "window-size", tr(app,
        "pages.preference.cat.labels.windowSize", "Window Size"), tr(app,
        "composables.useAppMenu.labels.wheelSizeHint", "Wheel: resize"),
        10.0f, &window_state->scale_percent, 500.0f, 1.0f,
        BONGO_CAT_DEFAULT_WINDOW_SCALE_PERCENT);
    if (old_scale != window_state->scale_percent && old_scale > 0.0f) {
        float requested_scale = window_state->scale_percent;
        window_state->scale_percent = old_scale;
        bongo_cat_window_cancel_wheel_animation(app);
        bongo_cat_window_set_scale(app, requested_scale);
    }
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_WINDOW_CORNERS);
    /* Display the fraction of maximum rounding; keep saved radii in their
       original units (percent of the short edge) for existing settings. */
    float corner_roundness = window->corner_radius_percent * 2.0f;
    if (bongo_cat_pref_toggle_float(context, "window-corners", tr(app,
        "pages.preference.cat.labels.windowCorners", "Window Corners (%)"),
        &window->rounded_corners, 0.0f, &corner_roundness,
        100.0f, 1.0f, BONGO_CAT_DEFAULT_WINDOW_CORNER_PERCENT * 2.0f)) {
        window->corner_radius_percent = corner_roundness * 0.5f;
        app->dirty = true;
        bongo_cat_window_mark_hit_dirty(app);
    }
    float old_opacity = window_state->opacity_percent;
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_OPACITY);
    bongo_cat_pref_slider(context, "opacity", tr(app,
        "pages.preference.cat.labels.opacity", "Opacity"), tr(app,
        "composables.useAppMenu.labels.wheelOpacityHint", "Ctrl+Wheel: opacity"),
        10.0f, &window_state->opacity_percent, 100.0f, 1.0f,
        BONGO_CAT_DEFAULT_WINDOW_OPACITY_PERCENT);
    if (old_opacity != window_state->opacity_percent)
        bongo_cat_window_cancel_wheel_animation(app);
    if (old_opacity != window_state->opacity_percent && !app->hover_hidden) {
        bongo_cat_app_cancel_hover_fade(app);
        bongo_cat_platform_set_opacity(&app->platform,
            window_state->opacity_percent / 100.0f);
    }
    section_gap(context, 10);
    bongo_cat_preferences_options_end(value, context);
    }
    if (bongo_cat_preferences_options_begin(value, context,
        BONGO_CAT_OPTIONS_PERFORMANCE,
        tr(app, "pages.preference.cat.labels.performanceSettings", "Performance"),
        BONGO_CAT_PREF_ICON_SECTION_APPEARANCE)) {
    {
        /* 超采样: 质量档 2x 边缘更干净; 2x 是 4 倍面积, 大窗口很吃显存 */
        const char *const labels[] = {
            tr(app, "pages.preference.cat.labels.ssAuto", "Auto"),
            tr(app, "pages.preference.cat.labels.ssQuality", "Quality"),
            tr(app, "pages.preference.cat.labels.ssMemory", "Low memory")};
        const int count = (int)(sizeof(labels) / sizeof(labels[0]));
        int selected = app->settings.window.supersample;
        if (selected < 0 || selected >= count) selected = 0;
        bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_RENDERER);
        /* 用下拉而不是主题那种图标分段控件: 这里要显示"自动/质量/省内存"文字 */
        selected = bongo_cat_pref_combo(context, "supersample",
            tr(app, "pages.preference.cat.labels.supersampleQuality",
                "Supersampling"),
            tr(app, "pages.preference.cat.hints.supersample",
                "自动: 窗口较大时退回 1x; 2x 是 4 倍面积, 更吃显存"),
            labels, count, selected);
        if (selected != app->settings.window.supersample) {
            app->settings.window.supersample = selected;
            app->dirty = true;
            bongo_cat_window_apply(app);
        }
    }
    {
        /* 关掉界面的发光/阴影: 浅色主题下那几项全屏模糊是每帧最贵的一项 */
        bool effects_off = app->settings.window.disable_effects != 0;
        bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_RENDERER);
        if (bongo_cat_pref_toggle(context, "disable-ui-effects", tr(app,
                "pages.preference.cat.labels.disableUiEffects",
                "Disable UI effects"),
                tr(app, "pages.preference.cat.labels.disableUiEffectsHint",
                    "Turn off glows and shadows for a smoother settings window"),
                &effects_off)) {
            app->settings.window.disable_effects = effects_off ? 1 : 0;
            app->dirty = true;
        }
    }
    {
        bool capped = app->settings.window.font_raster_cap != 0;
        bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_MAX_FPS);
        if (bongo_cat_pref_toggle(context, "font-raster-cap", tr(app,
                "pages.preference.cat.labels.fontRasterCap",
                "Cap font sharpness"),
                tr(app, "pages.preference.cat.labels.fontRasterCapHint",
                    "HiDPI 下字体图集限制在 2 倍, 更省内存"), &capped)) {
            app->settings.window.font_raster_cap = capped ? 1 : 0;
            app->dirty = true;
        }
    }    section_gap(context, 10);
    bongo_cat_preferences_options_end(value, context);
    }
    if (bongo_cat_preferences_options_begin(value, context,
        BONGO_CAT_OPTIONS_MODEL,
        tr(app, "pages.preference.cat.labels.modelSettings", "Model"),
        BONGO_CAT_PREF_ICON_SECTION_MODEL)) {
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_MIRROR);
    if (bongo_cat_pref_toggle(context, "mirror", tr(app,
        "pages.preference.cat.labels.mirrorMode", "Mirror Mode"), "",
        &model->mirror)) {
        app->model_pointer_anchor_ready = false;
        app->dirty = true;
    }
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_MOUSE_MIRROR);
    if (bongo_cat_pref_toggle(context, "mouse-mirror", tr(app,
        "pages.preference.cat.labels.mouseMirror", "Mouse Mirror"), "",
        &model->mouse_mirror)) {
        app->pointer_known = false;
        app->dirty = true;
    }
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_MOUSE_CENTERED);
    if (bongo_cat_pref_toggle(context, "mouse-centered", tr(app,
        "pages.preference.cat.labels.mouseCentered", "Mouse Centered on Desktop Pet"),
        "", &model->mouse_centered)) {
        bongo_cat_app_reset_pointer_tracking(app);
    }
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_IGNORE_MOUSE);
    if (bongo_cat_pref_toggle(context, "ignore-mouse", tr(app,
        "pages.preference.cat.labels.ignoreMouse", "Ignore Mouse Events"), "",
        &model->ignore_mouse)) {
        app->pointer_known = false;
        app->dirty = true;
    }
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_MAX_FPS);
    model->max_fps = bongo_cat_pref_fps(context, "max-fps", tr(app,
        "pages.preference.cat.labels.maxFPS", "Max Frame Rate"), model->max_fps,
        tr(app, "pages.preference.cat.labels.unlimitedFps", "Unlimited"));
    /* 渲染后端: 实现在 preferences_renderer_row.c (它要同时管"写配置 -> 确认框 ->
       重启"和"被拒也要留痕", 与托盘那一项共用 runtime 里的同一份实现)。 */
    bongo_cat_preferences_renderer_row(app, context);
    bongo_cat_preferences_options_end(value, context);
    }
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

static void page_general(BongoCatPreferences *value, struct nk_context *context) {
    BongoCatApp *app = value->app;
    BongoCatApplicationPreferences *options = &app->settings.app;
    // Keep each option in its own native language so the list is recognizable
    // regardless of the language currently used by the settings window.
    const char *ui_languages[] = {"简体中文", "繁體中文", "English",
        "Français", "Deutsch", "日本語", "한국어", "Português",
        "Русский", "Español"};
    if (bongo_cat_preferences_options_begin(value, context,
        BONGO_CAT_OPTIONS_APPLICATION,
        tr(app, "pages.preference.general.labels.appSettings", "Application"),
        BONGO_CAT_PREF_ICON_SECTION_APPLICATION)) {
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
    bongo_cat_preferences_portable_draw(value, context);
    bongo_cat_preferences_cache_path_draw(value, context);
    bongo_cat_preferences_options_end(value, context);
    }    section_gap(context, 7);
    if (bongo_cat_preferences_options_begin(value, context,
        BONGO_CAT_OPTIONS_APPEARANCE,
        tr(app, "pages.preference.general.labels.appearanceSettings", "Appearance"),
        BONGO_CAT_PREF_ICON_SECTION_APPEARANCE)) {
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
    bongo_cat_preferences_options_end(value, context);
    }
#ifdef __APPLE__
    section_gap(context, 7);
    input_monitoring_section(app, context);
#endif
}

void bongo_cat_preferences_page_settings(BongoCatPreferences *value,
    struct nk_context *context) {
    page_display(value, context);
    section_gap(context, 10);
    page_general(value, context);
}
/* 模型页的"随机"标签页: 随机表情/随机动作/顺序切换模型, 原来在设置页, 现在收纳到
   模型页顶部分段控件里 (类型 X 的模型商店)。开关右边那个设置按钮各自打开行为弹窗
   (动作/表情 tab), 顺序切换模型的设置按钮跳到模型页第一个标签重新拖拽顺序。 */
void bongo_cat_preferences_page_model_random(BongoCatPreferences *value,
    struct nk_context *context) {
    BongoCatApp *app = value->app;
    BongoCatWindowPreferences *window = &app->settings.window;
    if (bongo_cat_preferences_options_begin(value, context,
        BONGO_CAT_OPTIONS_RANDOM,
        tr(app, "pages.preference.cat.labels.randomSettings", "Random"),
        BONGO_CAT_PREF_ICON_RANDOM_EXPRESSION)) {
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_RANDOM_EXPRESSION);
    bool random_expression_config = false;
    bongo_cat_pref_toggle_float_config(context, "random-expression",
        tr(app, "pages.preference.cat.labels.randomExpression", "Random Expressions"),
        tr(app, "pages.preference.cat.labels.secondsUnit", "s"),
        &window->random_expression, 1.0f,
        &window->random_expression_interval_seconds, 3600.0f, 1.0f,
        BONGO_CAT_DEFAULT_RANDOM_EXPRESSION_SECONDS,
        tr(app, "pages.preference.cat.hints.randomExpression",
            "Pick a random expression at the set interval. Use the settings button "
            "to choose participating expressions."),
        &random_expression_config);
    if (random_expression_config) {
        value->behavior_tab = 1; /* 表情 tab */
        bongo_cat_preferences_behavior_dialog_open(value);
    }

    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_RANDOM_MOTION);
    bool random_motion_config = false;
    bongo_cat_pref_toggle_float_config(context, "random-motion",
        tr(app, "pages.preference.cat.labels.randomMotion", "Random Motions"),
        tr(app, "pages.preference.cat.labels.secondsUnit", "s"),
        &window->random_motion, 1.0f,
        &window->random_motion_interval_seconds, 3600.0f, 1.0f,
        BONGO_CAT_DEFAULT_RANDOM_MOTION_SECONDS,
        tr(app, "pages.preference.cat.hints.randomMotion",
            "Trigger a random motion at the set interval. Use the settings "
            "button to choose participating motions."),
        &random_motion_config);
    if (random_motion_config) {
        value->behavior_tab = 0; /* motion tab */
        bongo_cat_preferences_behavior_dialog_open(value);
    }

    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_MULTIPLE_MODELS);
    bool sequential_model_config = false;
    bongo_cat_pref_toggle_float_config(context, "sequential-model",
        tr(app, "pages.preference.cat.labels.sequentialModel",
            "Sequential Models"),
        tr(app, "pages.preference.cat.labels.secondsUnit", "s"),
        &window->sequential_model, 1.0f,
        &window->sequential_model_interval_seconds, 3600.0f, 1.0f,
        BONGO_CAT_DEFAULT_SEQUENTIAL_MODEL_SECONDS,
        tr(app, "pages.preference.cat.hints.sequentialModel",
            "Switch models in order at the set interval. Use the settings "
            "button to reorder models on the Model page."),
        &sequential_model_config);
    if (sequential_model_config) {
        value->model_page_tab = 0; /* 模型 tab: 拖拽顺序即轮换顺序 */
    }

    section_gap(context, 10);
    bongo_cat_preferences_options_end(value, context);
    }
}