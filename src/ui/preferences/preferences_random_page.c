/*
 * 模型页的"随机"标签页: 随机表情/随机动作/随机声音三个可展开分区 + 顺序切换模型分区
 * (从 preferences_pages.c 拆出来)。
 *
 * 为什么单独一个文件: 随机声音那一版给这一页加了"分区表头 + 参与清单 + 顺序轮换清单",
 * preferences_pages.c 因此超过 500 行的策略上限 (见 cmake/CheckLines.cmake)。这一页只读
 * app->behaviors / app->models 与几个开关值, 和 [窗口] 区没有共享状态, 正好搬出来。
 *
 * 两条容易踩空的地方:
 *   1. 参与清单跟随的是"当前定焦模型"(app->behaviors 就是当前模型的行为目录), 多宠同显
 *      时页面顶部那个标识就是用来明示"现在在配哪只桌宠"的;
 *   2. 行为行自带交互(改名/录键/试听), 所以整行不再做"点击切换参与", 参与判定交给行尾
 *      那个开关, 三类都给 —— 声音也要能单独排除, 而普通对话框里声音行没有开关。
 */
#include "preferences_internal.h"
#include "preferences_state.h"
#include "preferences_theme.h"
#include "preferences_controls.h"
#include "preferences_icons.h"
#include "ui_icons.h"
#include "bongo_cat/config.h"
#include "bongo_cat/i18n.h"
#include "bongo_cat/path.h"
#include "bongo_cat/preferences.h"

#include <stdio.h>
#include <string.h>

static const char *tr(BongoCatApp *app, const char *key, const char *fallback) {
    return bongo_cat_i18n_get(app->i18n, key, fallback);
}

static void section_gap(struct nk_context *context, float pixels) {
    context->current->layout->at_y += pixels;
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

/* 顺序模型分区: 图标 + 标题 + 间隔步进器 + 启用开关, 不再列出可逐项"跳过"的子项。 */
static void draw_sequential_model_category(BongoCatPreferences *value,
    struct nk_context *context) {
    BongoCatApp *app = value->app;
    BongoCatWindowPreferences *window = &app->settings.window;
    draw_category_header(value, context,
        BONGO_CAT_OPTIONS_RANDOM_SEQUENTIAL,
        BONGO_CAT_PREF_ICON_MULTIPLE_MODELS,
        tr(app, "pages.preference.cat.labels.sequentialModel",
            "Sequential Models"),
        "sequential-model", &window->sequential_model,
        &window->sequential_model_interval_seconds,
        BONGO_CAT_DEFAULT_SEQUENTIAL_MODEL_SECONDS);
}

/* 模型页的"随机"标签页: 表情/动作/声音 三个可展开分区, 每个展开后内嵌逐条可勾选的
   参与子项列表, 直接在页内改参与项; 底部是"顺序切换模型"分区, 只保留启动开关。 */
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