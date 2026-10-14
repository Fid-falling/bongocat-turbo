/*
 * 一键更新的状态条: 贴在设置窗口底部居中的一枚胶囊, 显示
 *   一行标题 (阶段 + 版本 + 百分比) 与一行细节 (当前下载源 / 手动替换的文件路径),
 *   下载时胶囊底边还有一条按百分比填充的进度条。
 *
 * 为什么不用浮动提示条 (preferences_notice.c): 那是 2.6 秒就消失的一次性回执, 而"正在
 * 下载""正在校验""准备重启"是**持续状态**, 失败原因也必须留到用户看明白为止。两者职责
 * 不同, 所以是两套控件: 提示条在顶部, 状态条在底部, 互不遮挡。
 *
 * 文案约定: 阶段标题里最多只有一个 %s (版本号), 百分比由本文件自己拼 —— 这样翻译只需要
 * 处理一个占位符, 少一类"某个语言写错格式串"的风险。找不到 key 时一律回退英文。
 */
#include "preferences_update_status.h"
#include "preferences_notice.h"
#include "ui_backend.h"
#include "ui_paint.h"
#include "update_service.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#define STATUS_MARGIN 22.0f
#define STATUS_PADDING 22.0f
#define STATUS_TITLE_HEIGHT 24.0f
#define STATUS_DETAIL_HEIGHT 20.0f
#define STATUS_BAR_HEIGHT 4.0f
#define STATUS_MIN_WIDTH 260.0f

static const char *tr(BongoCatPreferences *value, const char *key,
    const char *fallback) {
    return bongo_cat_i18n_get(value->app->i18n, key, fallback);
}

static float text_width(const struct nk_user_font *font, const char *text) {
    return font->width(font->userdata, font->height, text, nk_strlen(text));
}

/* 按 UTF-8 整字截断并补 "...": 状态条不换行, 过长的路径只能截断, 但绝不能截出半个汉字。 */
static void fit_text(const struct nk_user_font *font, const char *source,
    float maximum, char *output, size_t capacity) {
    snprintf(output, capacity, "%s", source ? source : "");
    if (text_width(font, output) <= maximum) return;
    size_t length = strlen(output);
    if (length > capacity - 4) length = capacity - 4;
    while (length) {
        length--;
        while (length && ((unsigned char)output[length] & 0xc0) == 0x80) length--;
        memcpy(output + length, "...", 4);
        if (text_width(font, output) <= maximum) return;
    }
    snprintf(output, capacity, "...");
}

/* 阶段标题与细节。返回 false 表示当前没有需要显示的状态 (IDLE/已是最新/有新版待点击等)。 */
static bool status_text(BongoCatPreferences *value,
    const BongoCatUpdateSnapshot *snapshot, char *title, size_t title_capacity,
    char *detail, size_t detail_capacity, unsigned *percent) {
    *percent = 0;
    detail[0] = '\0';
    switch (snapshot->status) {
    case BONGO_CAT_UPDATE_DOWNLOADING: {
        if (snapshot->total)
            *percent = (unsigned)((snapshot->downloaded * 100) / snapshot->total);
        char base[128];
        snprintf(base, sizeof(base), tr(value,
            "native.support.updateStatusDownloading", "Downloading v%s"),
            snapshot->release.version);
        if (snapshot->total)
            snprintf(title, title_capacity, "%s (%u%%)", base, *percent);
        else snprintf(title, title_capacity, "%s", base);
        if (snapshot->source_count)
            snprintf(detail, detail_capacity, tr(value,
                "native.support.updateStatusSource", "Source %u/%u: %s"),
                snapshot->source_index, snapshot->source_count,
                snapshot->source[0] ? snapshot->source : "?");
        return true;
    }
    case BONGO_CAT_UPDATE_VERIFYING:
        snprintf(title, title_capacity, "%s", tr(value,
            "native.support.updateStatusVerifying",
            "Verifying the downloaded update"));
        return true;
    case BONGO_CAT_UPDATE_INSTALLING:
        snprintf(title, title_capacity, "%s", tr(value,
            "native.support.updateStatusInstalling",
            "The update is ready; restarting BongoCat"));
        return true;
    case BONGO_CAT_UPDATE_ERROR:
        if (snapshot->manual_replace && snapshot->manual_path[0]) {
            snprintf(title, title_capacity, "%s", tr(value,
                "native.support.updateStatusManual",
                "The update must be finished manually"));
            snprintf(detail, detail_capacity, "%s", snapshot->manual_path);
            return true;
        }
        if (!snapshot->error[0] && !snapshot->release.version[0]) return false;
        snprintf(title, title_capacity, "%s", snapshot->error[0] ?
            bongo_cat_update_error_text(value->app->i18n, snapshot->error) :
            tr(value, "native.support.updateFailed",
                "Unable to check for updates"));
        return true;
    default:
        return false;
    }
}

void bongo_cat_preferences_update_status_draw(BongoCatPreferences *value,
    struct nk_context *context, float width, float height) {
    if (!value || !context || !value->app) return;
    BongoCatUpdateSnapshot snapshot;
    bongo_cat_update_snapshot(value->app->update, &snapshot);
    char title[256], detail[512];
    unsigned percent = 0;
    if (!status_text(value, &snapshot, title, sizeof(title), detail,
            sizeof(detail), &percent)) return;
    bool error = snapshot.status == BONGO_CAT_UPDATE_ERROR;
    BongoCatUIPalette p = bongo_cat_ui_palette(bongo_cat_ui_dark(context));
    const struct nk_user_font *title_font = value->ui.caption_font;
    const struct nk_user_font *detail_font = value->ui.caption_font;
    if (!title_font || !detail_font) return;
    float maximum = NK_MAX(120.0f, NK_MIN(width - 80.0f, 520.0f));
    char shown_title[256], shown_detail[512];
    fit_text(title_font, title, maximum, shown_title, sizeof(shown_title));
    float title_width = text_width(title_font, shown_title);
    float detail_width = 0.0f;
    if (detail[0]) {
        fit_text(detail_font, detail, maximum, shown_detail, sizeof(shown_detail));
        detail_width = text_width(detail_font, shown_detail);
    }
    float pill_width = NK_MAX(STATUS_MIN_WIDTH,
        NK_MAX(title_width, detail_width) + STATUS_PADDING * 2.0f);
    pill_width = NK_MIN(pill_width, width - 40.0f);
    float pill_height = detail[0] ? STATUS_TITLE_HEIGHT + STATUS_DETAIL_HEIGHT +
        STATUS_PADDING * 1.4f : STATUS_TITLE_HEIGHT + STATUS_PADDING * 1.2f;
    if (snapshot.status == BONGO_CAT_UPDATE_DOWNLOADING)
        pill_height += STATUS_BAR_HEIGHT;
    struct nk_rect bounds = nk_rect((width - pill_width) * .5f,
        height - STATUS_MARGIN - pill_height, pill_width, pill_height);
    if (bounds.x < 20.0f) bounds.x = 20.0f;
    struct nk_command_buffer *canvas = nk_window_get_canvas(context);
    struct nk_color tone = error ? p.danger : p.accent;
    if (p.effects) bongo_cat_ui_paint_shadow(context, bounds, 18, 0, 8, 18, 0,
        nk_rgba(tone.r, tone.g, tone.b, 77));
    nk_fill_rect(canvas, bounds, 18, p.surface);
    nk_stroke_rect(canvas, bounds, 18, 1,
        nk_rgba(tone.r, tone.g, tone.b, error ? 220 : 150));
    float text_x = bounds.x + (bounds.w - NK_MAX(title_width, detail_width)) * .5f;
    nk_draw_text(canvas, nk_rect(text_x, bounds.y + STATUS_PADDING * .6f,
        title_width + 1.0f, title_font->height), shown_title,
        nk_strlen(shown_title), title_font, nk_rgba(0, 0, 0, 0),
        error ? tone : p.text);
    if (detail[0]) {
        float detail_x = bounds.x + (bounds.w - detail_width) * .5f;
        nk_draw_text(canvas, nk_rect(detail_x, bounds.y + STATUS_PADDING * .6f +
            STATUS_TITLE_HEIGHT, detail_width + 1.0f, detail_font->height),
            shown_detail, nk_strlen(shown_detail), detail_font,
            nk_rgba(0, 0, 0, 0), p.muted);
    }
    if (snapshot.status == BONGO_CAT_UPDATE_DOWNLOADING) {
        /* 进度条贴在胶囊底边内侧: 没给 Content-Length 时用一条空槽表示"在下载, 不知道
           还要多久", 而不是画一条永远 0% 的假进度。 */
        struct nk_rect track = nk_rect(bounds.x + STATUS_PADDING * .5f,
            bounds.y + bounds.h - STATUS_BAR_HEIGHT - 6.0f,
            bounds.w - STATUS_PADDING, STATUS_BAR_HEIGHT);
        nk_fill_rect(canvas, track, STATUS_BAR_HEIGHT * .5f,
            nk_rgba(p.border_subtle.r, p.border_subtle.g, p.border_subtle.b, 160));
        if (snapshot.total) {
            track.w = NK_MAX(2.0f, track.w * (float)percent / 100.0f);
            nk_fill_rect(canvas, track, STATUS_BAR_HEIGHT * .5f, tone);
        }
    }
    /* 刻意**不**在这里标脏: 状态条可能长时间停在一个状态上 (失败原因要一直显示给用户
       看), 每帧标脏会让设置窗口永远重绘。进度变化由 preferences.c 的 needs_frame 轮询
       service 快照后发现, 只有真的变了才重画。 */
}
