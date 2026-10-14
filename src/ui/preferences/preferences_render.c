#include "preferences_state.h"
#include "preferences_render_internal.h"
#include "preferences_page_dispatch.h"
#include "preferences_controls.h"
#include "preferences_notice.h"
#include "preferences_update_status.h"
#include "ui_catime.h"
#include "ui_animation.h"
#include "ui_icons.h"
#include "ui_paint.h"
#include "bongo_cat/memory_policy.h"
#include "bongo_cat/tray.h"
#include "update_service.h"
#include <SDL3/SDL_opengl.h>
#include <math.h>
#include <stdio.h>
/*
 * 设置窗口的 UI 树构建: 外壳 (标题/侧栏/内容头) + 当前页面 + 各层弹窗。
 *
 * 这里有两套容易踩坑的状态, 改之前先看懂:
 * 1. 根窗口样式 (RootStyle) 是全局的 Nuklear 样式。draw_frame 会临时把它改成
 * "零内边距/零间距"来精确排外壳, 画完必须原样还原 —— 否则后续画弹窗、
 * 画轮盘时会继承这套零内边距, 布局全线错位;
 * 2. 滚动是"双位置"状态机: Nuklear 组滚动是实际值 (actual), scroll_target 是
 * 滚轮驱动的目标值, 每帧按帧时间做指数平滑逼近 (帧率无关)。用户拖滚动条时
 * 实际值会瞬间偏离目标值, 这时必须把目标值拉回实际值, 否则动画会和用户的
 * 手互相对抗。
 */
typedef struct RootStyle { struct nk_vec2 padding, group_padding, spacing;
    struct nk_style_item fixed; struct nk_color background; float group_border; } RootStyle;
/* 与 preferences_present.c 同一套开关: BONGO_CAT_TEST_PREFS_STAGES=1 时打印分段 */
static bool stages_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) enabled = SDL_getenv("BONGO_CAT_TEST_PREFS_STAGES") != NULL;
    return enabled != 0;
}

static void stage_log(const char *stage) {
    if (stages_enabled())
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "[prefs] stage=%s", stage);
}

static const char *tr(const BongoCatPreferences *value, const char *key, const char *fallback) {
    return bongo_cat_i18n_get(value->app->i18n, key, fallback);
}
static void draw_icon(void *userdata, struct nk_command_buffer *canvas, int icon,
    struct nk_rect bounds, struct nk_color color) {
    bongo_cat_preferences_icon_draw(userdata, canvas, icon, bounds, color); }
/* 只备份被 root_style_apply 改过的那几项; 多备份反而容易漏还原 */
static RootStyle root_style_save(struct nk_context *context) {
    RootStyle saved;
    saved.padding = context->style.window.padding;
    saved.group_padding = context->style.window.group_padding;
    saved.spacing = context->style.window.spacing;
    saved.fixed = context->style.window.fixed_background;
    saved.background = context->style.window.background;
    saved.group_border = context->style.window.group_border;
    return saved;
}
/*
 * 外壳按"像素级排版"来做, 所以把根窗口的内边距/间距全部清零, 由 BONGO_CAT_UI_MARGIN
 * 和 nk_layout_row_* 显式给间距。透明窗口时背景要透成全透明 (由合成器/桌面透出来)。
 */
static void root_style_apply(struct nk_context *context,
    BongoCatUIPalette palette, bool transparent) {
    context->style.window.padding = nk_vec2(BONGO_CAT_UI_MARGIN,
        BONGO_CAT_UI_MARGIN);
    context->style.window.group_padding = nk_vec2(0, 0);
    context->style.window.spacing = nk_vec2(0, 0);
    struct nk_color background = transparent ? nk_rgba(0, 0, 0, 0) :
        palette.background;
    context->style.window.fixed_background = nk_style_item_color(background);
    context->style.window.background = background;
    context->style.window.group_border = 0;
}
/* 必须成对调用: Nuklear 样式是全局的, 漏还原会污染之后画的每一个窗口 */
static void root_style_restore(struct nk_context *context,
    const RootStyle *saved) {
    context->style.window.padding = saved->padding;
    context->style.window.group_padding = saved->group_padding;
    context->style.window.spacing = saved->spacing;
    context->style.window.fixed_background = saved->fixed;
    context->style.window.background = saved->background;
    context->style.window.group_border = saved->group_border;
}
/*
 * 画外壳并把当前页塞进内容区。
 * modal 为真时页面整体变灰且吃掉点击/滚轮 (见下面的 input 保存-清除-还原);
 * 返回值是"内容头右上角的关闭按钮被点了", 由 present 在帧末真正关窗。
 */
static bool draw_shell(BongoCatPreferences *value, struct nk_context *context,
    float width, float height, bool dark) {
    static const char *page_ids[] = {
        "page-settings", "page-model", "page-shortcuts", "page-about"};
    static const int menu_icons[] = {
        BONGO_CAT_UI_ICON_CAT, BONGO_CAT_UI_ICON_WAND,
        BONGO_CAT_UI_ICON_KEYBOARD, BONGO_CAT_UI_ICON_HEART};
    const char *menus[] = {
        tr(value, "pages.preference.cat.title", "Preferences"),
        tr(value, "pages.preference.model.title", "Model"),
        tr(value, "pages.preference.shortcut.title", "Shortcuts"),
        tr(value, "native.support.title", "Support the Developer")};
    bool modal = bongo_cat_preferences_remove_dialog_active(value->app) ||
        bongo_cat_preferences_behavior_dialog_active(value);
    const bool native_chrome = bongo_cat_ui_native_chrome(); BongoCatUIPalette p = bongo_cat_ui_palette(dark);
    bongo_cat_ui_shell_draw(context, width, height, dark,
        !value->transparent_window);
    stage_log("shell-bg");
    float sidebar = bongo_cat_ui_sidebar_width(width);
    float interior_height = height - BONGO_CAT_UI_MARGIN * 2.0f;
    nk_layout_row_begin(context, NK_STATIC, interior_height, 2);
    nk_layout_row_push(context, sidebar);
    struct nk_color clear = nk_rgba(0, 0, 0, 0);
    context->style.window.fixed_background = nk_style_item_color(clear);
    context->style.window.background = clear;
    context->style.window.group_padding = nk_vec2(0, 0);
    bool title_clicked = false;
    if (nk_group_begin(context, "preferences-sidebar", NK_WINDOW_NO_SCROLLBAR)) {
    bongo_cat_ui_header(context, "BongoCat",
        value->ui.heading_font, value->logo_texture, &title_clicked, !modal, dark, native_chrome);
    if (title_clicked && !SDL_OpenURL("https://bongocat.pet"))
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot open website: %s", SDL_GetError());
    bongo_cat_ui_set_icons(draw_icon, value);
    BongoCatUpdateSnapshot update_snapshot;
    bongo_cat_update_snapshot(value->app->update, &update_snapshot);
    bool about_badge = update_snapshot.status == BONGO_CAT_UPDATE_AVAILABLE ||
        update_snapshot.status == BONGO_CAT_UPDATE_DOWNLOADING ||
        update_snapshot.status == BONGO_CAT_UPDATE_VERIFYING ||
        update_snapshot.status == BONGO_CAT_UPDATE_INSTALLING ||
        (update_snapshot.status == BONGO_CAT_UPDATE_ERROR &&
            update_snapshot.release.version[0]);
    bongo_cat_ui_tabs(context, menus, menu_icons, 4, &value->page,
        !modal, dark, interior_height, about_badge,
        draw_icon, value);
    if (!value->page_seen) {
        /* 第一帧只记录起始页, 不做切页处理: 否则会误清一次缓存 */
        value->page_seen = true; value->last_page = value->page;
    } else if (value->last_page != value->page) {
        /* 侧栏刚被点过: 先释放上一页独占的资源, 再启动一次淡入过渡 */
        bongo_cat_preferences_page_cache_clear(value,
            value->last_page, value->page);
        value->last_page = value->page;
        value->page_transition_ns = SDL_GetTicksNS();
        value->render_dirty = true;
    }
    nk_group_end(context);
    }
    stage_log("shell-sidebar");
    nk_layout_row_push(context, width - BONGO_CAT_UI_MARGIN * 2.0f - sidebar);
    bool close_requested = false;
    if (nk_group_begin(context, "preferences-content", NK_WINDOW_NO_SCROLLBAR)) {
    close_requested = bongo_cat_ui_content_header(context,
        menus[value->page], menu_icons[value->page], !modal, dark,
        native_chrome);
    if (close_requested) {
        /*
         * 已经进了两个组, 必须自己把布局栈收干净再返回 —— 少了这两句,
         * Nuklear 的行/组栈就不平衡, 之后每一帧的排版都会跟着错。
         * (滚动卡片会盖到内容头的命中区上, 所以只能在这里判, 不能等画完。)
         */
        /* Scrolled cards can overlap the header's hit area despite clipping. */
        nk_group_end(context);
        nk_layout_row_end(context);
        return true;
    }
    float body_height = interior_height - BONGO_CAT_UI_HEADER_HEIGHT;
    nk_layout_row_dynamic(context, NK_MAX(120.0f, body_height), 1);
    struct nk_rect body_bounds = nk_widget_bounds(context);
    float page_progress = 1.0f;
    if (value->page_transition_ns) {
        float elapsed = (float)(SDL_GetTicksNS() - value->page_transition_ns) /
            220000000.0f;
        if (elapsed >= 1.0f) value->page_transition_ns = 0;
        else {
            page_progress = bongo_cat_ui_ease(BONGO_CAT_UI_EASE_SWIFT,
                NK_CLAMP(0.0f, elapsed, 1.0f));
            value->render_dirty = true;
        }
    }
    context->style.window.group_padding = nk_vec2(24,
        16 + 6.0f * (1.0f - page_progress));
    context->style.window.spacing = nk_vec2(10, 10);
    /*
     * 滚动状态机 (每页各一份, 索引就是页号):
     * scroll_ready  首次读到该组的真实滚动值之前不许回写 —— 新组的滚动位置是 0,
     * 第一帧就回写会把用户上一轮留下的位置冲掉;
     * scroll_target 滚轮意图, 由 maximum 夹住;
     * maximum       从 Nuklear 当前 layout 的 at_y 推算内容高度: 库本身不给
     * "内容总高", 只能这样量; 拿不到 layout 时退化成 0 (不可滚)。
     */
    int scroll_page = NK_CLAMP(0, value->page, 3);
    bool scroll_animating = fabsf(value->scroll_current[scroll_page] - value->scroll_target[scroll_page]) > .5f;
    struct nk_style_scrollbar saved_scrollv = context->style.scrollv;
    struct nk_rect scrollbar_hit = nk_rect(body_bounds.x + body_bounds.w - 14, body_bounds.y, 14, body_bounds.h);
    bool scrollbar_hover = !modal && nk_input_is_mouse_hovering_rect(&context->input, scrollbar_hit);
    context->style.scrollv.padding.x = scrollbar_hover ? 0.0f : 2.0f;
    context->style.scrollv.cursor_hover = nk_style_item_color(p.accent);
    context->style.scrollv.cursor_active = nk_style_item_color(p.accent);
    if (scroll_animating)
        context->style.scrollv.cursor_normal = nk_style_item_color(p.accent);
    if (value->scroll_ready[scroll_page])
        nk_group_set_scroll(context, page_ids[scroll_page], 0,
            (nk_uint)NK_MAX(0.0f, value->scroll_current[scroll_page]));
    /*
     * 弹窗是画在同一个 Nuklear 窗口里的浮层, 底下那层页面并不知道自己被盖住了,
     * 会把"穿透"过来的点击和滚轮也吃掉。这里先备份再清零, 画完页面后还原,
     * 让输入只落到弹窗上。
     */
    struct nk_mouse_button saved_left = context->input.mouse.buttons[NK_BUTTON_LEFT];
    struct nk_vec2 saved_wheel = context->input.mouse.scroll_delta;
    if (modal) {
        context->input.mouse.buttons[NK_BUTTON_LEFT].clicked = 0;
        context->input.mouse.buttons[NK_BUTTON_LEFT].down = nk_false;
        context->input.mouse.scroll_delta = nk_vec2(0, 0);
    }
    if (nk_group_begin(context, page_ids[value->page], 0)) {
        bongo_cat_preferences_draw_page(value, context);
        float wheel = context->input.mouse.scroll_delta.y;
        struct nk_panel *layout = context->current->layout;
        float maximum = layout ? NK_MAX(0.0f, layout->at_y + layout->row.height - layout->bounds.y - layout->bounds.h) : 0.0f;
        if (wheel != 0.0f) context->style.scrollv.cursor_normal =
            nk_style_item_color(p.accent);
        /* 滚轮已被本页消费, 清零否则后面的浮层/控件会再滚一次 */
        context->input.mouse.scroll_delta.y = 0;
        nk_group_end(context);
        context->style.scrollv = saved_scrollv;
        nk_uint group_x = 0, group_y = 0;
        nk_group_get_scroll(context, page_ids[value->page], &group_x, &group_y);
        float actual = (float)group_y;
        if (!value->scroll_ready[scroll_page]) {
            /*
             * 本页第一次被看到: 以库里的真实值为初值。注意不能在这个分支之外
             * 用 scroll_target 去回写, 那会把别页残留的目标值当成这一页的。
             */
            value->scroll_ready[scroll_page] = true;
            value->scroll_current[scroll_page] = actual;
            value->scroll_target[scroll_page] = actual;
        }
        value->scroll_target[scroll_page] = NK_CLAMP(0.0f, value->scroll_target[scroll_page], maximum);
        if (wheel != 0.0f)
            value->scroll_target[scroll_page] = NK_CLAMP(0.0f,
                value->scroll_target[scroll_page] - wheel * 72.0f, maximum);
        else if (fabsf(actual - value->scroll_current[scroll_page]) > 2.0f)
            /*
             * 实际值与记录值拉开超过 2px, 只可能是用户自己拖了滚动条 (动画不可能
             * 走这么快), 于是把目标值对齐到用户停下的地方, 免得松手后被动画拽回去。
             */
            value->scroll_target[scroll_page] = NK_CLAMP(0.0f,
                actual, maximum);
        /* 指数平滑, 用帧时间做系数: 60Hz 和 144Hz 下的手感才一致 */
        float blend = 1.0f - expf(-context->delta_time_seconds / .035f);
        float next = actual +
            (value->scroll_target[scroll_page] - actual) * blend;
        if (fabsf(next - value->scroll_target[scroll_page]) < .5f) next =
            value->scroll_target[scroll_page];
        value->scroll_current[scroll_page] = NK_CLAMP(0.0f, next, maximum);
        bool still_animating = fabsf(value->scroll_current[scroll_page] -
            value->scroll_target[scroll_page]) > .5f;
        if (wheel != 0.0f || still_animating ||
            (scroll_animating && !still_animating))
            value->render_dirty = true;
    } else context->style.scrollv = saved_scrollv;
    stage_log("shell-body");
    if (modal) {
        context->input.mouse.buttons[NK_BUTTON_LEFT] = saved_left;
        context->input.mouse.scroll_delta = saved_wheel;
    }
    if (page_progress < 1.0f) {
        /* 切页过渡: 用一层随进度变淡的底色盖住内容, 掩盖页面瞬间换掉的突兀感 */
        struct nk_command_buffer *canvas = nk_window_get_canvas(context);
        nk_fill_rect(canvas, body_bounds, 0, nk_rgba(p.surface.r,
            p.surface.g, p.surface.b,
            (nk_byte)(255 * (1.0f - page_progress))));
    }
    nk_group_end(context);
    }
    nk_layout_row_end(context);
    context->style.window.fixed_background = nk_style_item_color(p.surface);
    context->style.window.background = p.surface;
    bongo_cat_preferences_update_status_draw(value, context, width, height);
    bongo_cat_preferences_notice_draw(value, context, width, height);
    bongo_cat_preferences_remove_dialog_draw(value->app, context);
    bongo_cat_preferences_behavior_dialog_draw(value, context);
    return close_requested;
}
/*
 * 一帧的入口。根样式改了又还原, 是因为 Nuklear 没有样式作用域, 只能用
 * "进函数保存、出函数还原" 的约定。
 */
bool bongo_cat_preferences_draw_frame(BongoCatPreferences *value,
    float width, float height,
    bool dark) {
    struct nk_context *context = &value->ui.context;
    RootStyle saved = root_style_save(context);
    BongoCatUIPalette palette = bongo_cat_ui_palette(dark);
    root_style_apply(context, palette, value->transparent_window);
    bool close_requested = false;
    if (nk_begin(context, BONGO_CAT_NAME,
        nk_rect(0, 0, (float)width, (float)height), NK_WINDOW_NO_SCROLLBAR))
        close_requested = draw_shell(value, context, width, height, dark);
    nk_end(context);
    root_style_restore(context, &saved);
    return close_requested;
}
