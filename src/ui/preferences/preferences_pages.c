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
            /* 关掉拖动惯性 = 立即停下当前滑行; 打开时什么都不用做 (下一帧就按新值走)。 */
            if (!window->slide_animation) bongo_cat_window_stop_inertia(app);
            app->dirty = true;
        }
    }
    /*
     * 关闭动量衰减: 只在"拖动惯性"打开时显示这一行 —— 关掉惯性时它不生效, 显示出来
     * 只会让人以为点了没反应。注意这里**不动** window->slide_no_decay: 隐藏不等于重置,
     * 用户重新打开拖动惯性就接着用原来的选择。
     * 文案刻意用大白话 (一直滑动 / 撞到边缘弹回来), 不用"动量""衰减系数"这类说法。
     */
    if (window->slide_animation) {
        bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_FRAME_BOUNDS);
        if (bongo_cat_pref_toggle(context, "slide-no-decay", tr(app,
            "pages.preference.cat.labels.slideNoDecay", "Keep Sliding"),
            tr(app, "pages.preference.cat.hints.slideNoDecay",
                "After you let go it keeps sliding and bounces off the screen "
                "edges. Grab it again to stop it."),
            &window->slide_no_decay)) {
            /* 关掉这一项 = 立即停下"永不停止"的滑行 (打开不用做什么: 下一帧就按新值走,
               当前速度不变, 于是从这一刻起开始衰减)。 */
            if (!window->slide_no_decay) bongo_cat_window_stop_inertia(app);
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

void bongo_cat_preferences_page_settings(BongoCatPreferences *value,
    struct nk_context *context) {
    page_display(value, context);
    section_gap(context, 10);
    bongo_cat_preferences_general_page(value, context);
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
