#include "preferences_internal.h"
#include "preferences_state.h"
#include "preferences_theme.h"
#include "preferences_widgets.h"
#include "preferences_controls.h"
#include "preferences_notice.h"
#include "ui_icons.h"
#include "preferences_overlay.h"
#include "ui_tooltip.h"
#include "bongo_cat/audio.h"
#include "bongo_cat/path.h"
#include "bongo_cat/config.h"
#include "bongo_cat/i18n.h"
#include "bongo_cat/log.h"
#include "bongo_cat/overlay.h"
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

/* [位置] 区 (窗口页): 置顶 / 穿透 / 悬停 / 屏内 / 吸附 / 惯性。 */
static void window_position_section(BongoCatPreferences *value,
    struct nk_context *context) {
    BongoCatApp *app = value->app;
    BongoCatWindowPreferences *window = &app->settings.window;
    if (bongo_cat_preferences_options_begin(value, context,
        BONGO_CAT_OPTIONS_POSITION,
        tr(app, "pages.preference.cat.labels.positionSettings", "Position"),
        BONGO_CAT_PREF_ICON_SECTION_MODEL)) {
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_ALWAYS_ON_TOP);
    if (bongo_cat_pref_toggle(context, "always-top", tr(app,
        "composables.useAppMenu.labels.alwaysOnTop", "Always on Top"), "",
        &window->always_on_top)) {
        bongo_cat_platform_set_always_on_top(&app->platform, window->always_on_top);
        bongo_cat_window_mark_hit_dirty(app);
        bongo_cat_window_sync_click_through(app);
    }
    bongo_cat_ui_question_tooltip(context, tr(app,
        "pages.preference.cat.hints.gameInput", "Pet not responding in games?"),
        tr(app, "pages.preference.cat.hints.gameInputHelp",
            "Try running BongoCat as administrator and setting the game to windowed mode."));
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
    bongo_cat_preferences_options_end(value, context);
    }
}
/* [样式] 区 (窗口页): 尺寸 / 圆角 / 不透明度。 */
static void window_style_section(BongoCatPreferences *value,
    struct nk_context *context) {
    BongoCatApp *app = value->app;
    BongoCatWindowPreferences *window = &app->settings.window;
    BongoCatWindowState *window_state = &app->session.window;
    if (bongo_cat_preferences_options_begin(value, context,
        BONGO_CAT_OPTIONS_STYLE,
        tr(app, "pages.preference.cat.labels.styleSettings", "Style"),
        BONGO_CAT_PREF_ICON_SECTION_WINDOW)) {
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
    bongo_cat_preferences_options_end(value, context);
    }
}

/* [背景] 区 (窗口页): 窗口在桌面上怎么呈现 —— 仅录屏显示 / 实体背景 / 透明背景。 */
static void window_background_section(BongoCatPreferences *value,
    struct nk_context *context) {
    BongoCatApp *app = value->app;
    BongoCatWindowPreferences *window = &app->settings.window;
    if (bongo_cat_preferences_options_begin(value, context,
        BONGO_CAT_OPTIONS_BACKGROUND,
        tr(app, "pages.preference.cat.labels.backgroundSettings", "Background"),
        BONGO_CAT_PREF_ICON_SECTION_WINDOW)) {
    /* 只在录屏/直播软件里显示: 仅 Windows 有等价机制 (DWM 隐藏), 其它平台不显示
       这一项, 免得让用户点一个没反应的开关。 */
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
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_SOLID_BACKGROUND);
    if (bongo_cat_pref_obs_background(context, "obs-background", tr(app,
        "pages.preference.cat.labels.obsBackground", "Solid Background"), tr(app,
        "pages.preference.cat.hints.obsBackground", "Window capture is black?"), tr(app,
        "pages.preference.cat.hints.obsBackgroundHelp", "OBS: enable this option, use "
        "the Windows 7 compatibility method, and remove the background with a "
        "color key filter."),
        &window->obs_background, &window->obs_background_color))
        app->dirty = true;
    /* 透明背景: 只有 Windows 有等价机制, 其它平台连定义都没有, 调用点跟着条件编译。 */
#ifdef _WIN32
    bongo_cat_preferences_window_transparency(app, context);
#endif
    /* 鼠标穿透: 透明背景的下方。 */
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_PASS_THROUGH);
    if (bongo_cat_pref_toggle(context, "pass-through", tr(app,
        "composables.useAppMenu.labels.passThrough", "Pass Through"), tr(app,
        "pages.preference.cat.hints.passThrough",
        "You can also turn this off by right-clicking the tray icon"),
        &window->pass_through)) {
        bongo_cat_window_mark_hit_dirty(app);
        bongo_cat_window_sync_click_through(app);
    }
    bongo_cat_preferences_options_end(value, context);
    }
}

/* [性能] 区 (左子页)。 */
static void performance_section(BongoCatPreferences *value,
    struct nk_context *context) {
    BongoCatApp *app = value->app;
    if (bongo_cat_preferences_options_begin(value, context,
        BONGO_CAT_OPTIONS_PERFORMANCE,
        tr(app, "pages.preference.cat.labels.performanceSettings", "Performance"),
        BONGO_CAT_PREF_ICON_SECTION_APPEARANCE)) {
    /*
     * 帧节奏: 这一帧怎么出来 —— 先按什么节奏出帧 (最大帧率), 再跟不跟显示器刷新 (垂直同步),
     * 最后才是用哪个后端把它画出来 (渲染后端)。这三行回答的是同一个问题, 用户调整顺序时也是
     * 连着看的, 所以紧挨着按这个先后放 (垂直同步排在「最大帧率」与「渲染后端」之间)。
     * 开=锁在显示器刷新率上(FIFO / swap interval 1), 关=按上面的"最大帧率"限帧(IMMEDIATE /
     * swap interval 0); 两种状态下"最大帧率"都仍然是天花板。语义的唯一权威在 frame_clock.c 的
     * bongo_cat_app_apply_frame_pacing(), 这里的提示文案必须和它一致。
     *
     * 显示的是**收口后**的值: 老配置里 sentinel(-1)已经由 settings_validate 按"同步跟随帧率"
     * 的老规则解成了 0/1, 所以界面看到的就是当前真实生效的状态; 用户一拨动就写成显式 0/1,
     * 从此不再依赖那条兼容规则。
     */
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_MAX_FPS);
    app->settings.model.max_fps = bongo_cat_pref_fps(context, "max-fps",
        tr(app, "pages.preference.cat.labels.maxFPS", "Max Frame Rate"),
        app->settings.model.max_fps,
        tr(app, "pages.preference.cat.labels.unlimitedFps", "Unlimited"));
    bool perf_vsync = app->settings.model.vsync != 0;
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_VSYNC);
    if (bongo_cat_pref_toggle(context, "vsync", tr(app,
            "pages.preference.cat.labels.vsync", "V-Sync"),
            tr(app, "pages.preference.cat.hints.vsync",
                "让画面与显示器刷新同步, 减少撕裂; 关闭后按上面的最大帧率运行"),
            &perf_vsync)) {
        app->settings.model.vsync = perf_vsync ? 1 : 0;
        app->dirty = true;
    }
    /* 渲染后端: 实现在 preferences_renderer_row.c (它要同时管"写配置 -> 确认框 ->
       重启"和"被拒也要留痕", 与托盘那一项共用 runtime 里的同一份实现)。
       排在上面两行之后: 帧率与垂直同步决定"这一帧什么时候出", 后端决定"用什么画",
       先有节奏再选工具的顺序读起来才顺 (Live2D 的 Core 状态行也跟着这一行走)。 */
    bongo_cat_preferences_renderer_row(app, context);
    /*
     * 渲染精度: 以 N 倍分辨率渲染这一帧, 再线性缩回窗口尺寸 (两个后端都支持, 见
     * gl_scale.c / vk_scale.c)。1.0 = 与窗口 1:1, 也就是 1.17.0 的行为; 控件步进
     * 0.1、显示一位小数, 双击回到默认值。
     */
    {
        float render_scale = app->settings.model.render_scale;
        bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_RENDERER);
        if (bongo_cat_pref_float(context, "render-scale", tr(app,
                "pages.preference.cat.labels.renderScale", "Render scale"),
                tr(app, "pages.preference.cat.labels.renderScaleHint",
                    "越大边缘越平滑、越吃显卡；1.0 与以前一致，补不回贴图细节"),
                BONGO_CAT_RENDER_SCALE_MIN, &render_scale,
                BONGO_CAT_RENDER_SCALE_MAX, BONGO_CAT_RENDER_SCALE_STEP,
                BONGO_CAT_RENDER_SCALE_DEFAULT)) {
            app->settings.model.render_scale = render_scale;
            app->dirty = true;
            /* 立即下发: GL 每帧读设置, VK 要先把倍率交给呈现层 (见 shell/window.c) */
            bongo_cat_window_apply(app);
        }
    }

    /* 画质取舍: 每一帧要画多精致、花多少资源 —— 三档都能下调来省显存。 */
    /* 纹理质量 (模型图集上限) 单独一行, 见 preferences_texture_quality_row.c */
    bongo_cat_preferences_texture_quality_row(app, context);
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
    }
    bongo_cat_preferences_options_end(value, context);
    }
}

/* [镜像] 区 (右子页)。模型水平/垂直镜像、手势水平/垂直镜像, 从 [模型] 区单独拆出的组合镜像。 */
static void mirror_section(BongoCatPreferences *value, struct nk_context *context) {
    BongoCatApp *app = value->app;
    BongoCatModelPreferences *model = &app->settings.model;
    if (bongo_cat_preferences_options_begin(value, context,
        BONGO_CAT_OPTIONS_MIRROR,
        tr(app, "pages.preference.cat.labels.mirrorSection", "Mirror"),
        BONGO_CAT_PREF_ICON_SECTION_MODEL)) {
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_MIRROR);
    if (bongo_cat_pref_toggle(context, "mirror", tr(app,
        "pages.preference.cat.labels.mirrorMode",
        "Model Horizontal Mirror"), "", &model->mirror)) {
        app->model_pointer_anchor_ready = false;
        app->pointer_known = false;
        app->dirty = true;
    }
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_VERTICAL_FLIP);
    if (bongo_cat_pref_toggle(context, "vertical-flip", tr(app,
        "pages.preference.cat.labels.verticalMirror",
        "Model Vertical Mirror"), "", &model->vertical_flip))
        bongo_cat_app_reset_pointer_tracking(app);
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_MOUSE_MIRROR);
    if (bongo_cat_pref_toggle(context, "mouse-mirror", tr(app,
        "pages.preference.cat.labels.mouseMirror",
        "Hand Horizontal Mirror"), "", &model->mouse_mirror)) {
        app->pointer_known = false;
        app->dirty = true;
    }
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_MOUSE_VERTICAL_FLIP);
    if (bongo_cat_pref_toggle(context, "mouse-vertical-flip", tr(app,
        "pages.preference.cat.labels.mouseVerticalMirror",
        "Hand Vertical Mirror"), "", &model->mouse_vertical_flip))
        bongo_cat_app_reset_pointer_tracking(app);
    bongo_cat_preferences_options_end(value, context);
    }
}

/* [输入] 区 (右子页)。镜像相关已拆到 [镜像] 分区, 这里只留指针/输入相关的选项。 */
static void input_section(BongoCatPreferences *value, struct nk_context *context) {
    BongoCatApp *app = value->app;
    BongoCatModelPreferences *model = &app->settings.model;
    if (bongo_cat_preferences_options_begin(value, context,
        BONGO_CAT_OPTIONS_MODEL,
        tr(app, "pages.preference.cat.labels.inputSettings", "Input"),
        BONGO_CAT_PREF_ICON_SECTION_MODEL)) {
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
    {
        /*
         * 多键显示: 同一侧同时按住的键全部画出来 (一只手 + N 个高亮键帽), 而不是只留
         * 最后按下的那一个。关掉即退回旧行为 —— 每侧只画最早按下的键。
         */
        bool multi = model->multi_key_display != 0;
        bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_MULTIPLE_MODELS);
        if (bongo_cat_pref_toggle(context, "multi-key-display", tr(app,
                "pages.preference.cat.labels.multiKeyDisplay",
                "Show multiple keys per hand"),
                tr(app, "pages.preference.cat.labels.multiKeyDisplayHint",
                    "同侧多键时显示所有按下的键帽"), &multi)) {
            model->multi_key_display = multi ? 1 : 0;
            app->dirty = true;
            bongo_cat_overlay_set_key_display(app->overlay, multi,
                model->multi_key_limit);
        }
        /*
         * 多键显示的画法: 合并成一个气泡 (里面用 "+" 连起键名) 还是每个键各占一个气泡。
         * 关掉多键显示时这一行没有意义, 不画 —— 只对自带气泡素材的模型有区别, 内置模型
         * 两种取值下都是"一只手 + N 块彩色键帽", 但选项本身仍然可调。
         */
        if (multi) {
            bool merge = model->bubble_merge != 0;
            bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_MULTIPLE_MODELS);
            if (bongo_cat_pref_toggle(context, "bubble-merge", tr(app,
                    "pages.preference.cat.labels.bubbleMerge",
                    "Merge into one bubble"),
                    tr(app, "pages.preference.cat.labels.bubbleMergeHint",
                        "同侧多键只画一个气泡, 用 + 连接键名"), &merge)) {
                model->bubble_merge = merge ? 1 : 0;
                app->dirty = true;
                bongo_cat_overlay_set_bubble_merge(app->overlay, merge);
            }
        }
    }
    bongo_cat_preferences_options_end(value, context);
    }
}

/*
 * 偏好设置页正文 (侧栏「偏好设置」那一页)。页顶的切页条把它分成三个子页, 由
 * value->preferences_tab 选择 (切页逻辑见 preferences_page_dispatch.c):
 *   0 = 应用页 (App): 应用 / 界面 —— 程序怎么跑、长什么样;
 *   1 = 性能页 (Perf): 超采样 / UI效果 / 字体锐化 / 最大帧率 / 垂直同步 / 渲染后端;
 *   2 = 窗口页 (Window): 位置 / 样式 / 背景 —— 桌宠窗口的位置、观感与背景;
 *   3 = 互动页 (Interaction): 输入 / 镜像 / 权限 —— 桌宠本身怎么响应交互、怎么显示。
 * 三个子页共用同一个页面组与滚动槽 (下标 0), 切子页时由切页条把滚动复位到顶部。
 * [应用]/[界面]/[权限] 三区定义在 preferences_general_page.c (见那里的行数策略说明)。
 */
void bongo_cat_preferences_page_settings(BongoCatPreferences *value,
    struct nk_context *context) {
    if (value->preferences_tab == 0) {
        bongo_cat_preferences_application_section(value, context);
        section_gap(context, 10);
        bongo_cat_preferences_appearance_section(value, context);
        return;
    }
    if (value->preferences_tab == 1) {
        performance_section(value, context);
        return;
    }
    if (value->preferences_tab == 2) {
        window_position_section(value, context);
        section_gap(context, 10);
        window_style_section(value, context);
        section_gap(context, 10);
        window_background_section(value, context);
        return;
    }
    input_section(value, context);
    section_gap(context, 10);
    mirror_section(value, context);
    section_gap(context, 10);
    bongo_cat_preferences_permissions_section(value, context);
}
