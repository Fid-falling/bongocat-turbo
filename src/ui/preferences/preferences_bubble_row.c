/*
 * 按键气泡相关的三行 (「屏蔽按键气泡 / 同侧多键显示 / 合并为一个气泡」)。
 *
 * 为什么单独一个文件: preferences_pages.c 已经贴着 500 行的策略上限 (见 cmake/CheckLines.cmake),
 * 而这三行是一个整体 —— 开关之间互斥, 改一个有连带动作 —— 拆出来之后页面文件只留一行调用,
 * 与 preferences_texture_quality_row.c 从页面里拆出去是同一个理由。
 *
 * 互斥规则: 屏蔽气泡时, 下面两行"只渲染、不响应输入"(enabled=false), 但**不清空它们的取值** ——
 * 用户关掉屏蔽之后按原样恢复, 否则用户的选择会被吞掉。
 */
#include "preferences_internal.h"

#include "preferences_widgets.h"

#include "bongo_cat/i18n.h"
#include "bongo_cat/overlay.h"
#include "bongo_cat/preferences.h"

#include <SDL3/SDL.h>

static const char *tr(BongoCatApp *app, const char *key, const char *fallback) {
    return app && app->i18n ? bongo_cat_i18n_get(app->i18n, key, fallback)
        : fallback;
}

void bongo_cat_preferences_bubble_rows(BongoCatApp *app,
    struct nk_context *context) {
    if (!app) return;
    BongoCatModelPreferences *model = &app->settings.model;
    bool hidden = model->bubble_hidden != 0;
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_MULTIPLE_MODELS);
    if (bongo_cat_pref_toggle(context, "hide-bubble", tr(app,
            "pages.preference.cat.labels.hideBubble", "Hide key bubbles"),
            tr(app, "pages.preference.cat.labels.hideBubbleHint",
                "对自带气泡的模型不显示按键气泡"), &hidden)) {
        model->bubble_hidden = hidden ? 1 : 0;
        app->dirty = true;
        bongo_cat_overlay_set_bubble_hidden(app->overlay, hidden);
        bongo_cat_overlay_set_key_display(app->overlay,
            model->multi_key_display != 0, model->multi_key_limit);
        bongo_cat_overlay_set_bubble_merge(app->overlay,
            model->bubble_merge != 0);
    }
    /* 互斥的另一半: 屏蔽气泡期间下面两行只渲染、不响应输入 */
    const bool enabled = model->bubble_hidden == 0;
    bool multi = model->multi_key_display != 0;
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_MULTIPLE_MODELS);
    if (bongo_cat_pref_toggle_enabled(context, "multi-key-display", tr(app,
            "pages.preference.cat.labels.multiKeyDisplay",
            "Show multiple keys per hand"),
            tr(app, "pages.preference.cat.labels.multiKeyDisplayHint",
                "同侧多键时显示所有按下的键帽"), &multi, enabled)) {
        model->multi_key_display = multi ? 1 : 0;
        app->dirty = true;
        bongo_cat_overlay_set_key_display(app->overlay, multi,
            model->multi_key_limit);
    }
    /* 关掉多键显示时这一行没有意义, 不画。 */
    if (!multi) return;
    bool merge = model->bubble_merge != 0;
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_MULTIPLE_MODELS);
    if (bongo_cat_pref_toggle_enabled(context, "bubble-merge", tr(app,
            "pages.preference.cat.labels.bubbleMerge", "Merge into one bubble"),
            tr(app, "pages.preference.cat.labels.bubbleMergeHint",
                "同侧多键只画一个气泡, 用 + 连接键名"), &merge, enabled)) {
        model->bubble_merge = merge ? 1 : 0;
        app->dirty = true;
        bongo_cat_overlay_set_bubble_merge(app->overlay, merge);
    }
}
