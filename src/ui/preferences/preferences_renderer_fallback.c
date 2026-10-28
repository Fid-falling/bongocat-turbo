/*
 * 设置 → 性能 → 「渲染后端」上方的那条自动回退提示。
 *
 * 存在理由: 首帧看门狗自动回退 OpenGL 之后, 用户以前只能在日志里看到这件事 —— 界面上
 * 什么都不知道, 于是下次还会手动改回 Vulkan 再卡一次。这里把"发生过什么、为什么、怎么
 * 再试一次"放在渲染后端那一行正上方:
 *   - 主文案: Vulkan 在这台机器上启动失败, 已自动改用 OpenGL;
 *   - 小字:   原因 (首帧超时/初始化失败) · 连续次数 · 最近时间 · 显卡;
 *   - 暂停时多一句"已暂停尝试 Vulkan";
 *   - 右侧按钮「再试一次 Vulkan」: 走共享的 bongo_cat_app_switch_renderer —— 它会清空
 *     自动回退计数、把配置设回 Vulkan 并自动重启, 与用户手动点分段控件完全同一条路。
 *
 * 只在**有回退记录**时出现 (bongo_cat_renderer_fallback_recorded): 正常机器上这一行
 * 不占任何空间, 26 项自检与页面高度都保持不变。
 *
 * 为什么自己做一张卡片而不是复用 bongo_cat_pref_button: 这是一条"需要被看见"的警告,
 * 普通表单行的视觉权重不够; 而卡片 + 右侧按钮的绘制原语在控件层没有现成的。绘制顺序
 * 与 preferences_update_status.c 的胶囊一致: 先背景, 再文字, 最后按钮命中检测。
 */
#include "preferences_internal.h"

#include "renderer_fallback_state.h"
#include "runtime.h"
#include "ui_backend.h"
#include "ui_catime.h"

#include "bongo_cat/i18n.h"
#include "bongo_cat/log.h"
#include "bongo_cat/render.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#define FALLBACK_TEXT_CAP 512
#define FALLBACK_DETAIL_CAP 1024
#define FALLBACK_RETRY_WIDTH 190.0f
#define FALLBACK_RETRY_HEIGHT 36.0f

static const char *tr(BongoCatApp *app, const char *key, const char *fallback) {
    return app && app->i18n ? bongo_cat_i18n_get(app->i18n, key, fallback)
        : fallback;
}

static float text_width(const struct nk_user_font *font, const char *text) {
    return font->width(font->userdata, font->height, text, nk_strlen(text));
}

/* 按 UTF-8 整字截断并补 "...": 卡片里不换行的小字用它, 绝不能截出半个汉字。 */
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

/* 主文案最多折两行: 十种语言的标题长度差一倍 (德语/俄语尤其长), 截断会把"已自动改用
   OpenGL"这句唯一结论吃掉。返回实际行数 (0..2)。 */
static int wrap_title(const struct nk_user_font *font, const char *text,
    float width, char lines[2][FALLBACK_TEXT_CAP]) {
    lines[0][0] = lines[1][0] = '\0';
    if (!text || !text[0] || width <= 0.0f) return 0;
    int rows = 0;
    size_t used = 0;
    float line_width = 0.0f;
    for (const char *cursor = text; *cursor && rows < 2;) {
        nk_rune rune = 0;
        int remaining = (int)NK_MIN((size_t)4, strlen(cursor));
        int length = nk_utf_decode(cursor, &rune, remaining);
        if (length <= 0) length = 1;
        bool newline = rune == '\n';
        float advance = newline ? 0.0f :
            font->width(font->userdata, font->height, cursor, length);
        if (newline || (used > 0 && line_width + advance > width)) {
            lines[rows][used] = '\0';
            rows++;
            used = 0;
            line_width = 0.0f;
            if (newline) { cursor += length; continue; }
            if (rows >= 2) break;
        }
        if (used + (size_t)length < FALLBACK_TEXT_CAP) {
            memcpy(lines[rows] + used, cursor, (size_t)length);
            used += (size_t)length;
            line_width += advance;
        }
        cursor += length;
    }
    if (used > 0 && rows < 2) {
        lines[rows][used] = '\0';
        rows++;
    }
    return rows;
}

/* 一条小字: "原因：首帧超时 · 连续 2 次 · 最近一次 <时间>"。三个片段各自可翻译,
   代码只负责用 " · " 串起来 —— 避免把参数顺序塞进翻译串 (十种语言里最容易出错的地方)。 */
static void detail_line(BongoCatApp *app,
    const BongoCatRendererFallbackState *state, char *output, size_t capacity) {
    const bool init_failed = strcmp(state->last_reason,
        BONGO_CAT_RENDERER_FALLBACK_REASON_INIT) == 0;
    const char *reason_text = init_failed
        ? tr(app, "pages.preference.cat.labels.rendererFallbackReasonInit",
            "Vulkan initialization failed")
        : tr(app, "pages.preference.cat.labels.rendererFallbackReasonTimeout",
            "First-frame timeout (no frame within 8 s)");
    char reason[FALLBACK_TEXT_CAP], failures[256], last[256];
    snprintf(reason, sizeof(reason), tr(app,
        "pages.preference.cat.labels.rendererFallbackReason", "Reason: %s"),
        reason_text);
    snprintf(failures, sizeof(failures), tr(app,
        "pages.preference.cat.labels.rendererFallbackFailures",
        "%d consecutive failures"), state->consecutive_failures);
    snprintf(last, sizeof(last), tr(app,
        "pages.preference.cat.labels.rendererFallbackLastAt", "Last attempt: %s"),
        state->last_failure_at[0] ? state->last_failure_at : "-");
    snprintf(output, capacity, "%s · %s · %s", reason, failures, last);
}

/* 第二条小字: 暂停说明 + 显卡。显卡摘要可能很长 (设备/驱动/api), 由绘制端截断。 */
static void status_line(BongoCatApp *app,
    const BongoCatRendererFallbackState *state, char *output, size_t capacity) {
    const char *paused = state->auto_disabled
        ? tr(app, "pages.preference.cat.labels.rendererFallbackPaused",
            "Automatic Vulkan attempts are paused") : "";
    snprintf(output, capacity, "%s%s%s", paused,
        paused[0] && state->last_gpu[0] ? " · " : "", state->last_gpu);
}

void bongo_cat_preferences_renderer_fallback(BongoCatApp *app,
    struct nk_context *context) {
    if (!app || !context) return;
    if (!bongo_cat_renderer_fallback_recorded(app)) return;
    const BongoCatRendererFallbackState *state = &app->renderer_fallback;
    const char *title = tr(app,
        "pages.preference.cat.labels.rendererFallbackTitle",
        "Vulkan failed to start on this machine; switched to OpenGL automatically");
    const char *retry_label = tr(app,
        "pages.preference.cat.labels.rendererFallbackRetry", "Try Vulkan again");
    char detail[FALLBACK_DETAIL_CAP], status[FALLBACK_DETAIL_CAP];
    detail_line(app, state, detail, sizeof(detail));
    status_line(app, state, status, sizeof(status));

    const struct nk_user_font *title_font = bongo_cat_ui_label_font(context);
    const struct nk_user_font *body_font = bongo_cat_ui_caption_font(context);
    if (!title_font || !body_font) return;
    BongoCatUIPalette p = bongo_cat_ui_palette(bongo_cat_ui_dark(context));
    float available = nk_window_get_content_region(context).w;
    if (available < 340.0f) available = 340.0f;
    float text_area = available - 42.0f - FALLBACK_RETRY_WIDTH - 24.0f;
    if (text_area < 200.0f) text_area = 200.0f;
    char title_lines[2][FALLBACK_TEXT_CAP];
    const int title_rows = wrap_title(title_font, title, text_area, title_lines);
    if (title_rows <= 0) return;
    float height = 14.0f + (float)title_rows * title_font->height + 6.0f +
        body_font->height * 2.0f + 14.0f;
    if (height < FALLBACK_RETRY_HEIGHT + 24.0f)
        height = FALLBACK_RETRY_HEIGHT + 24.0f;
    nk_layout_row_dynamic(context, height, 1);
    struct nk_rect row;
    if (nk_widget(&row, context) == NK_WIDGET_INVALID) return;

    struct nk_command_buffer *canvas = nk_window_get_canvas(context);
    nk_fill_rect(canvas, row, 14.0f, p.danger_background);
    nk_stroke_rect(canvas, row, 14.0f, 1.0f,
        nk_rgba(p.danger.r, p.danger.g, p.danger.b, 120));
    /* 左侧色条: 与分区标题左侧那一条同一语言, 让"这是一条警示"一眼可辨。 */
    nk_fill_rect(canvas, nk_rect(row.x, row.y + 10.0f, 4.0f,
        NK_MAX(8.0f, row.h - 20.0f)), 2.0f, p.danger);
    float text_x = row.x + 20.0f;
    float y = row.y + 12.0f;
    for (int i = 0; i < title_rows; ++i) {
        nk_draw_text(canvas, nk_rect(text_x, y, text_area, title_font->height),
            title_lines[i], nk_strlen(title_lines[i]), title_font,
            nk_rgba(0, 0, 0, 0), p.text);
        y += title_font->height;
    }
    y += 4.0f;
    char shown[FALLBACK_DETAIL_CAP];
    fit_text(body_font, detail, text_area, shown, sizeof(shown));
    nk_draw_text(canvas, nk_rect(text_x, y, text_area, body_font->height),
        shown, nk_strlen(shown), body_font, nk_rgba(0, 0, 0, 0), p.muted);
    y += body_font->height;
    if (status[0]) {
        fit_text(body_font, status, text_area, shown, sizeof(shown));
        nk_draw_text(canvas, nk_rect(text_x, y, text_area, body_font->height),
            shown, nk_strlen(shown), body_font, nk_rgba(0, 0, 0, 0), p.muted);
    }

    /* 按钮: 鼠标命中用 nk_input 的矩形判定 (与设置页其它自绘按钮一致), 点击完成后再走
       共享切换路径 —— 它负责清空计数、写配置、落盘与自动重启。 */
    const struct nk_rect button = nk_rect(
        row.x + row.w - 14.0f - FALLBACK_RETRY_WIDTH,
        row.y + (row.h - FALLBACK_RETRY_HEIGHT) * .5f,
        FALLBACK_RETRY_WIDTH, FALLBACK_RETRY_HEIGHT);
    const bool hover = nk_input_is_mouse_hovering_rect(&context->input, button);
    nk_fill_rect(canvas, button, 10.0f, hover ? p.danger : p.surface);
    nk_stroke_rect(canvas, button, 10.0f, 1.0f, p.danger);
    char shown_label[160];
    fit_text(title_font, retry_label, FALLBACK_RETRY_WIDTH - 20.0f, shown_label,
        sizeof(shown_label));
    const float label_width = text_width(title_font, shown_label);
    nk_draw_text(canvas, nk_rect(button.x + (button.w - label_width) * .5f,
        button.y + (button.h - title_font->height) * .5f,
        label_width + 1.0f, title_font->height), shown_label,
        nk_strlen(shown_label), title_font, nk_rgba(0, 0, 0, 0),
        hover ? nk_rgb(255, 255, 255) : p.danger);
    if (hover)
        bongo_cat_ui_cursor_hover_rect(context, button,
            BONGO_CAT_UI_CURSOR_POINTER);
    if (!hover || !nk_input_is_mouse_click_in_rect(&context->input,
            NK_BUTTON_LEFT, button)) return;
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[render] 设置提示条: 用户点「再试一次 Vulkan」(之前连续 %d 次自动回退, "
        "暂停=%d), 清空记录并把渲染后端设回 Vulkan",
        state->consecutive_failures, state->auto_disabled ? 1 : 0);
    bongo_cat_app_switch_renderer(app, BONGO_CAT_RENDERER_VULKAN,
        BONGO_CAT_RENDERER_ENTRY_SETTINGS);
}
