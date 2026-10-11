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

static void page_display(BongoCatApp *app, struct nk_context *context) {
    BongoCatModelPreferences *model = &app->settings.model;
    BongoCatWindowPreferences *window = &app->settings.window;
    BongoCatWindowState *window_state = &app->session.window;
    bongo_cat_pref_section_icon(context, tr(app,
        "pages.preference.cat.labels.windowSettings",
        "Window"),
        BONGO_CAT_PREF_ICON_SECTION_WINDOW);
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
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_RANDOM_EXPRESSION);
    bongo_cat_pref_toggle_float(context, "random-expression", tr(app,
        "pages.preference.cat.labels.randomExpression", "Random Expressions"),
        &window->random_expression, 1.0f,
        &window->random_expression_interval_seconds, 3600.0f, 1.0f,
        BONGO_CAT_DEFAULT_RANDOM_EXPRESSION_SECONDS);

    section_gap(context, 10);
    bongo_cat_pref_section_icon(context, tr(app,
        "pages.preference.cat.labels.performanceSettings", "Performance"),
        BONGO_CAT_PREF_ICON_SECTION_APPEARANCE);
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
    bongo_cat_pref_section_icon(context, tr(app,
        "pages.preference.cat.labels.modelSettings", "Model"),
        BONGO_CAT_PREF_ICON_SECTION_MODEL);
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
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_RENDERER);
    BongoCatRenderer previous_renderer = model->renderer;
    model->renderer = (BongoCatRenderer)bongo_cat_pref_renderer(context,
        "renderer", tr(app, "pages.preference.cat.labels.renderer", "Renderer"),
        (int)model->renderer,
        bongo_cat_renderer_available(BONGO_CAT_RENDERER_VULKAN));
    if (model->renderer != previous_renderer) {
        /* 后端在宠物窗口创建时一次性选定, 运行中换不了, 所以明确告诉用户要重启,
           免得选了 Vulkan 却看着还是 OpenGL 的画面。 */
        bongo_cat_preferences_notice_show(app, tr(app,
            "pages.preference.cat.hints.rendererRestart",
            "Renderer changes take effect after restarting BongoCat"), false);
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[render] 渲染后端偏好改为 %s, 需要重启才会生效",
            bongo_cat_renderer_name(model->renderer));
    }
}

static void update_autostart(BongoCatApp *app, bool old_value) {
    BongoCatError error = {0};
    if (bongo_cat_platform_set_autostart(app->settings.app.autostart, &error) == BONGO_CAT_OK) return;
    app->settings.app.autostart = old_value;
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", error.message);
    bongo_cat_preferences_notice_show(app, tr(app,
        "pages.preference.general.hints.autostartFailed",
        "Unable to update launch-on-startup settings"), true);
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

static void page_general(BongoCatApp *app, struct nk_context *context) {
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
    {
        /* 不勾选就以管理员身份运行 (启动时请求提权); 勾上完全跳过, 开机自启才可用 */
        bool no_admin = app->settings.window.disable_elevation != 0;
        bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_RENDERER);
        if (bongo_cat_pref_toggle(context, "disable-elevation", tr(app,
                "pages.preference.cat.labels.disableElevation",
                "Run without administrator"),
                tr(app, "pages.preference.cat.labels.disableElevationHint",
                    "Stops asking for administrator rights at startup; auto-start keeps working"),
                &no_admin)) {
            app->settings.window.disable_elevation = no_admin ? 1 : 0;
            app->dirty = true;
        }
    }
    if (bongo_cat_pref_toggle(context, "autostart", tr(app,
        "pages.preference.general.labels.launchOnStartup", "Launch on Startup"), "",
        &options->autostart)) update_autostart(app, old_autostart);
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

void bongo_cat_preferences_page_settings(BongoCatPreferences *value,
    struct nk_context *context) {
    BongoCatApp *app = value->app;
    page_display(app, context);
    section_gap(context, 10);
    page_general(app, context);
    section_gap(context, 10);
    bongo_cat_preferences_cache_path_draw(value, context);
}
