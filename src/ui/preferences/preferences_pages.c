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
/* 随机页某分类展开后的参与子项列表: 直接复用行为弹窗的"行为行" — 名字(可改名) +
   快捷键录制 + 试听/停 + 参与开关(三类都给, 声音也要能单独排除), 和弹窗共一份
   random_disabled 参与集合, 两边改动自动同步。因为行内自带交互(改名/录键/试听),
   "点整行切换参与"就不再做了, 改由行尾那个开关负责。 */
static void draw_participation_rows(BongoCatPreferences *value,
    struct nk_context *context, BongoCatBehaviorKind kind) {
    BongoCatApp *app = value->app;
    BongoCatUIPalette p =
        bongo_cat_ui_palette(bongo_cat_ui_dark(context));
    for (size_t i = 0; i < app->behaviors.count; ++i) {
        BongoCatBehaviorEntry *entry = &app->behaviors.entries[i];
        if (entry->kind != kind) continue;
        if (kind == BONGO_CAT_BEHAVIOR_SOUND && !entry->sound[0]) continue;
        nk_layout_row_dynamic(context, 56, 1);
        struct nk_rect row;
        if (!nk_widget(&row, context)) continue;
        struct nk_command_buffer *canvas = nk_window_get_canvas(context);
        bongo_cat_preferences_behavior_row_participation_draw(value, context,
            canvas, row, entry, p, 1.0f, true);
    }
}

/* 分区表头(随机表情/随机动作/随机声音/顺序模型共用): 把分隔表头、启用开关和间隔
 * 秒数并成一行 —— 图标 + 标题 + 间隔步进器 + 启用开关, 不再画折叠箭头。点击标题区
 * (避开步进器与开关)在展开/折叠之间切换, 开关直接改启用状态。返回当前是否展开,
 * 由调用方决定展开后画什么内容。 */
static bool draw_category_header(BongoCatPreferences *value,
    struct nk_context *context, enum BongoCatOptionsSection section,
    BongoCatPrefIcon icon, const char *title, const char *id,
    bool *enabled, float *interval, float default_seconds) {
    BongoCatUIPalette p = bongo_cat_ui_palette(bongo_cat_ui_dark(context));
    bool open = value->options_sections[section];
    nk_layout_row_dynamic(context, 56, 1);
    struct nk_rect row;
    if (nk_widget(&row, context) == NK_WIDGET_INVALID) return false;
    struct nk_command_buffer *canvas = nk_window_get_canvas(context);
    nk_fill_rect(canvas, row, 12, open ? p.selection : p.surface);
    nk_stroke_rect(canvas, row, 12, 1,
        open ? p.border : p.border_subtle);
    bongo_cat_pref_icon_draw(canvas, nk_rect(row.x + 14, row.y + 18, 20, 20),
        icon, p.accent);
    const struct nk_user_font *font = bongo_cat_ui_label_font(context);
    float toggle_width = 64.0f, input_width = 122.0f, gap = 10.0f;
    struct nk_rect toggle_cell = nk_rect(row.x + row.w - 14 - toggle_width,
        row.y + (row.h - 28) * .5f, toggle_width, 28);
    struct nk_rect interval_rect = nk_rect(toggle_cell.x - gap - input_width,
        row.y + (row.h - 34) * .5f, input_width, 34);
    struct nk_rect title_rect = nk_rect(row.x + 46,
        row.y + (row.h - font->height) * .5f,
        interval_rect.x - row.x - 54, font->height);
    nk_draw_text(canvas, title_rect, title, nk_strlen(title), font,
        nk_rgba(0, 0, 0, 0), p.text);
    if (bongo_cat_pref_control_float_rect(context, id, 1.0f, interval,
        3600.0f, 1.0f, default_seconds, interval_rect))
        value->render_dirty = true;
    if (bongo_cat_pref_control_toggle_rect(context, id, enabled,
        toggle_cell, true))
        value->render_dirty = true;
    bool in_control = nk_input_is_mouse_hovering_rect(&context->input,
        interval_rect) ||
        nk_input_is_mouse_hovering_rect(&context->input, toggle_cell);
    struct nk_rect title_hit = nk_rect(row.x, row.y,
        interval_rect.x - row.x, row.h);
    bool hover = !in_control &&
        nk_input_is_mouse_hovering_rect(&context->input, title_hit);
    if (hover) bongo_cat_ui_cursor_hover_rect(context, row,
        BONGO_CAT_UI_CURSOR_POINTER);
    if (!in_control && nk_input_is_mouse_click_in_rect(&context->input,
        NK_BUTTON_LEFT, title_hit)) {
        open = !open;
        value->options_sections[section] = open;
        value->render_dirty = true;
    }
    return open;
}

/* 随机页某分类(表情/动作/声音)的表头: 把分隔表头、启用开关和间隔秒数并成一行 ——
   图标 + 标题 + 间隔步进器 + 启用开关, 不再画折叠箭头。点击标题区(避开步进器与
   开关)在展开/折叠 "参与子项列表" 之间切换, 开关直接改启用状态。 */
static void draw_random_category(BongoCatPreferences *value,
    struct nk_context *context, enum BongoCatOptionsSection section,
    BongoCatBehaviorKind kind, BongoCatPrefIcon icon,
    const char *title, const char *id, bool *enabled,
    float *interval, float default_seconds) {
    bool open = draw_category_header(value, context, section, icon, title,
        id, enabled, interval, default_seconds);
    if (open) {
        draw_participation_rows(value, context, kind);
        section_gap(context, 8);
        nk_layout_row_dynamic(context, 6, 1);
        nk_spacing(context, 1);
    }
}

/* 顺序模型展开后的"当前顺序"列表: 按模型目录顺序逐行显示, 每行 = 序号 + 模型名 +
   一个"跳过"开关。开着 = 顺序轮换时跳过这个模型 (写入 switch:<id> 参与集合); 模型
   文件不存在时开关置灰且不可点。 */
static void draw_sequential_order_rows(BongoCatPreferences *value,
    struct nk_context *context) {
    BongoCatApp *app = value->app;
    BongoCatUIPalette p = bongo_cat_ui_palette(bongo_cat_ui_dark(context));
    const BongoCatSettings *settings = &app->settings;
    struct nk_command_buffer *canvas = nk_window_get_canvas(context);
    const struct nk_user_font *font = value->ui.caption_font;
    nk_layout_row_dynamic(context, 56, 1);
    for (size_t i = 0; i < app->models.count; ++i) {
        const BongoCatModelEntry *entry = &app->models.entries[i];
        struct nk_rect row;
        if (nk_widget(&row, context) == NK_WIDGET_INVALID) continue;
        bool exists =
            (entry->setting_file[0] &&
             bongo_cat_path_is_file(entry->setting_file)) ||
            (entry->directory[0] &&
             bongo_cat_path_is_dir(entry->directory));
        char switch_id[128];
        snprintf(switch_id, sizeof(switch_id), "switch:%s", entry->id);
        bool skip = bongo_cat_settings_random_disabled(settings, switch_id);
        bool collides = exists &&
            nk_input_is_mouse_hovering_rect(&context->input, row);
        nk_fill_rect(canvas, row, 10, collides ? p.hover : p.surface);
        char num[8]; snprintf(num, sizeof(num), "%zu", i + 1);
        struct nk_rect badge = nk_rect(row.x + 14, row.y + 18, 20, 20);
        nk_fill_rect(canvas, badge, 10, exists ? p.field : p.border_subtle);
        float numw = font->width(font->userdata, font->height, num, 1);
        nk_draw_text(canvas, nk_rect(badge.x + (badge.w - numw) * .5f,
            badge.y + (badge.h - font->height) * .5f, numw + 1, font->height),
            num, 1, font, nk_rgba(0, 0, 0, 0), p.muted);
        const char *name = entry->display_name[0] ? entry->display_name
            : entry->id;
        struct nk_rect name_rect = nk_rect(row.x + 46, row.y + 9,
            row.w - 130, 38);
        nk_draw_text(canvas, name_rect, name, nk_strlen(name), font,
            nk_rgba(0, 0, 0, 0), exists ? p.text : p.muted);
        struct nk_rect sw_cell = nk_rect(row.x + row.w - 56, row.y - 6,
            56, 68);
        if (bongo_cat_pref_control_toggle_rect(context, switch_id, &skip,
            sw_cell, exists)) {
            snprintf(value->pending_behavior_id,
                sizeof(value->pending_behavior_id), "%s", switch_id);
            value->pending_behavior_toggle = true;
            value->render_dirty = true;
        }
    }
    section_gap(context, 8);
    nk_layout_row_dynamic(context, 6, 1);
    nk_spacing(context, 1);
}

/* 随机页顶部的"正在配置哪只桌宠"标识: 随机页下方的参与清单跟随当前定焦模型
   (app->behaviors 就是当前模型的行为目录), 多宠同时显示时用它明示当前在编辑谁。 */
static void draw_focus_pet_indicator(BongoCatPreferences *value,
    struct nk_context *context) {
    BongoCatApp *app = value->app;
    BongoCatUIPalette p = bongo_cat_ui_palette(bongo_cat_ui_dark(context));
    const char *name = NULL;
    for (size_t i = 0; i < app->models.count; ++i) {
        const BongoCatModelEntry *entry = &app->models.entries[i];
        if (entry->id[0] && app->session.active_model_id[0] &&
            !strcmp(entry->id, app->session.active_model_id)) {
            name = entry->display_name[0] ? entry->display_name : entry->id;
            break;
        }
    }
    if (!name) name = tr(app, "pages.preference.model.random.defaultPet",
        "Current Pet");
    nk_layout_row_dynamic(context, 46, 1);
    struct nk_rect row;
    if (nk_widget(&row, context) == NK_WIDGET_INVALID) return;
    struct nk_command_buffer *canvas = nk_window_get_canvas(context);
    nk_fill_rect(canvas, row, 10, p.selection);
    nk_stroke_rect(canvas, row, 10, 1, p.border_subtle);
    nk_fill_rect(canvas, nk_rect(row.x + 15, row.y + 20.0f, 6, 6), 3, p.accent);
    const struct nk_user_font *cf = bongo_cat_ui_caption_font(context);
    const struct nk_user_font *lf = bongo_cat_ui_label_font(context);
    const char *label = tr(app, "pages.preference.model.random.configuring",
        "Configuring");
    float name_w = lf->width(lf->userdata, lf->height, name, nk_strlen(name));
    float lw = cf->width(cf->userdata, cf->height, label, nk_strlen(label));
    nk_draw_text(canvas, nk_rect(row.x + 31, row.y + (row.h - cf->height) * .5f,
        lw, cf->height), label, nk_strlen(label), cf,
        nk_rgba(0, 0, 0, 0), p.muted);
    /* 标签在左、桌宠名在右, 中间留白, 不用 ": / ·" 等连接符。 */
    nk_draw_text(canvas,
        nk_rect(row.x + row.w - 20 - name_w, row.y + (row.h - lf->height) * .5f,
            name_w, lf->height), name, nk_strlen(name), lf,
        nk_rgba(0, 0, 0, 0), p.accent);
}

/* 顺序模型分区 (新式表头): 图标 + 标题 + 间隔步进器 + 启用开关, 展开后列出当前
   轮换顺序并可在每一项上单独"跳过"。 */
static void draw_sequential_model_category(BongoCatPreferences *value,
    struct nk_context *context) {
    BongoCatApp *app = value->app;
    BongoCatWindowPreferences *window = &app->settings.window;
    bool open = draw_category_header(value, context,
        BONGO_CAT_OPTIONS_RANDOM_SEQUENTIAL,
        BONGO_CAT_PREF_ICON_MULTIPLE_MODELS,
        tr(app, "pages.preference.cat.labels.sequentialModel",
            "Sequential Models"),
        "sequential-model", &window->sequential_model,
        &window->sequential_model_interval_seconds,
        BONGO_CAT_DEFAULT_SEQUENTIAL_MODEL_SECONDS);
    if (open) draw_sequential_order_rows(value, context);
}

/* 模型页的"随机"标签页: 表情/动作/声音 三个可展开分区, 每个展开后内嵌逐条可勾选的
   参与子项列表, 直接在页内改参与项; 底部是"顺序切换模型"分区, 展开后列出轮换顺序
   并可逐项"跳过"。 */
void bongo_cat_preferences_page_model_random(BongoCatPreferences *value,
    struct nk_context *context) {
    BongoCatApp *app = value->app;
    BongoCatWindowPreferences *window = &app->settings.window;
    draw_focus_pet_indicator(value, context);
    section_gap(context, 6);
    draw_random_category(value, context, BONGO_CAT_OPTIONS_RANDOM_EXPRESSION,
        BONGO_CAT_BEHAVIOR_EXPRESSION, BONGO_CAT_PREF_ICON_RANDOM_EXPRESSION,
        tr(app, "pages.preference.cat.labels.randomExpression", "Random Expressions"),
        "random-expression", &window->random_expression,
        &window->random_expression_interval_seconds,
        BONGO_CAT_DEFAULT_RANDOM_EXPRESSION_SECONDS);
    draw_random_category(value, context, BONGO_CAT_OPTIONS_RANDOM_MOTION,
        BONGO_CAT_BEHAVIOR_MOTION, BONGO_CAT_PREF_ICON_RANDOM_MOTION,
        tr(app, "pages.preference.cat.labels.randomMotion", "Random Motions"),
        "random-motion", &window->random_motion,
        &window->random_motion_interval_seconds,
        BONGO_CAT_DEFAULT_RANDOM_MOTION_SECONDS);
    draw_random_category(value, context, BONGO_CAT_OPTIONS_RANDOM_SOUND,
        BONGO_CAT_BEHAVIOR_SOUND, BONGO_CAT_PREF_ICON_RANDOM_SOUND,
        tr(app, "pages.preference.cat.labels.randomSound", "Random Sounds"),
        "random-sound", &window->random_sound,
        &window->random_sound_interval_seconds,
        BONGO_CAT_DEFAULT_RANDOM_SOUND_SECONDS);

    draw_sequential_model_category(value, context);
}