/*
 * 设置 → 应用 → [运行自检]: 那一行开关 + 结果面板。
 *
 * 位置是用户指定的: 「便携模式」之后、「缓存目录」之前 —— 这三行说的都是"程序怎么跑 /
 * 数据放哪", 自检正好接着它们往后读。
 *
 * 面板是**设置窗口内的模态浮层** (与删除确认框同一套 overlay 开关动画), 而不是新窗口:
 * 新窗口会再建一条 GL/Vulkan 上下文, 在用户这台机器上正好是我们要诊断的东西, 诊断
 * 工具自己先踩一遍没有意义。面板内的列表可以滚动 (滚轮), 每项一行: 通过/警告/失败 +
 * 一句话说明; 底部是复制报告 / 保存为文件, 需要时多一个 [切换并重启]。
 *
 * 三个交互上的坑:
 *  1. 打开面板的那一次点击会穿透到面板上, 所以按钮判定必须经过
 *     bongo_cat_preferences_overlay_input_ready 的 input_armed;
 *  2. 面板画在页面之后, 页面滚动代码已经把滚轮消费掉了 —— 模态时 draw_shell 会先把
 *     输入清零再还原, 面板在还原之后画, 所以这里能拿到滚轮;
 *  3. 复制/保存的结果不用浮动提示 (提示条画在面板下面会被盖住), 直接在面板底部显示。
 */
#include "preferences_internal.h"
#include "preferences_model_glyphs.h"
#include "preferences_notice.h"
#include "preferences_overlay.h"
#include "preferences_state.h"
#include "preferences_widgets.h"
#include "ui_backend.h"
#include "bongo_cat/config.h"
#include "bongo_cat/i18n.h"
#include "bongo_cat/path.h"
#include "bongo_cat/render.h"
#include "bongo_cat/selfcheck.h"
#include "runtime.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

typedef struct BongoCatSelfCheckDialog {
    BongoCatApp *app;
    bool open;
    bool input_armed;
    uint64_t opened_ns;
    uint64_t closing_ns;
    float scroll;
    bool saved;
    char saved_path[BONGO_CAT_PATH_CAP];
    char saved_error[192];
    char feedback[96];
    uint64_t feedback_until_ns;
    BongoCatSelfCheckReport report;
} BongoCatSelfCheckDialog;

/* 面板是进程内单例, 用 app 指针标明归属 (与删除确认框同一条约定: 冒烟会建多个 app) */
static BongoCatSelfCheckDialog selfcheck;

static const char *tr(BongoCatApp *app, const char *key, const char *fallback) {
    return bongo_cat_i18n_get(app->i18n, key, fallback);
}

static struct nk_color alpha(struct nk_color color, float amount) {
    return bongo_cat_preferences_overlay_alpha(color, amount);
}

static void draw_text(struct nk_command_buffer *canvas, struct nk_rect bounds,
    const char *value, const struct nk_user_font *font, struct nk_color color) {
    nk_draw_text(canvas, bounds, value, nk_strlen(value), font,
        nk_rgba(0, 0, 0, 0), color);
}

static void draw_centered(struct nk_command_buffer *canvas,
    struct nk_rect bounds, const char *value,
    const struct nk_user_font *font, struct nk_color color) {
    float width = font->width(font->userdata, font->height, value,
        nk_strlen(value));
    draw_text(canvas, nk_rect(bounds.x + (bounds.w - width) * .5f,
        bounds.y + (bounds.h - font->height) * .5f, width + 1, font->height),
        value, font, color);
}

static bool hit(struct nk_context *context, struct nk_rect bounds,
    bool enabled) {
    return enabled && nk_input_is_mouse_hovering_rect(&context->input, bounds) &&
        nk_input_is_mouse_click_in_rect(&context->input, NK_BUTTON_LEFT, bounds);
}

/* 状态色: 通过/警告用固定的绿与琥珀 (两个主题下都够亮), 失败沿用主题的危险色 */
static struct nk_color status_color_for(BongoCatSelfCheckStatus status,
    BongoCatUIPalette p) {
    switch (status) {
    case BONGO_CAT_SELFCHECK_PASS: return nk_rgb(0x2e, 0xb8, 0x5c);
    case BONGO_CAT_SELFCHECK_WARN: return nk_rgb(0xe8, 0x9c, 0x1c);
    default: return p.danger;
    }
}

static const char *status_label(BongoCatApp *app, BongoCatSelfCheckStatus status) {
    switch (status) {
    case BONGO_CAT_SELFCHECK_PASS:
        return tr(app, "pages.preference.general.selfcheck.status.pass", "Pass");
    case BONGO_CAT_SELFCHECK_WARN:
        return tr(app, "pages.preference.general.selfcheck.status.warn", "Warning");
    default:
        return tr(app, "pages.preference.general.selfcheck.status.fail", "Failed");
    }
}

/*
 * 按像素宽度折行 (UTF-8 整字断行)。返回实际占用的行数, 最多 max_lines 行 ——
 * 面板一行放不下就截断, 不把后面的条目挤出去; 完整文本仍然在"复制报告"里。
 */
static int wrap_text(struct nk_command_buffer *canvas, float x, float y,
    float width, float line_height, const char *value,
    const struct nk_user_font *font, struct nk_color color, int max_lines) {
    char line[512];
    size_t used = 0;
    float line_width = 0.0f;
    int rows = 0;
    for (const char *cursor = value; *cursor && rows < max_lines;) {
        nk_rune rune = 0;
        int remaining = (int)NK_MIN((size_t)4, strlen(cursor));
        int length = nk_utf_decode(cursor, &rune, remaining);
        if (length <= 0) length = 1;
        bool newline = rune == '\n';
        float advance = newline ? 0.0f :
            font->width(font->userdata, font->height, cursor, length);
        if (newline || (used > 0 && line_width + advance > width)) {
            line[used] = '\0';
            draw_text(canvas, nk_rect(x, y + rows * line_height, width,
                line_height), line, font, color);
            rows++;
            used = 0;
            line_width = 0.0f;
            if (newline) { cursor += length; continue; }
            if (rows >= max_lines) break;
        }
        if (used + (size_t)length < sizeof(line)) {
            memcpy(line + used, cursor, (size_t)length);
            used += (size_t)length;
            line_width += advance;
        }
        cursor += length;
    }
    if (used > 0 && rows < max_lines) {
        line[used] = '\0';
        draw_text(canvas, nk_rect(x, y + rows * line_height, width, line_height),
            line, font, color);
        rows++;
    }
    return rows;
}

/* 预览时数出同样的行数 (只量不画): 滚动范围与条目位置都靠它 */
static int measure_text(const struct nk_user_font *font, float width,
    const char *value, int max_lines) {
    char line[512];
    size_t used = 0;
    float line_width = 0.0f;
    int rows = 0;
    for (const char *cursor = value; *cursor && rows < max_lines;) {
        nk_rune rune = 0;
        int remaining = (int)NK_MIN((size_t)4, strlen(cursor));
        int length = nk_utf_decode(cursor, &rune, remaining);
        if (length <= 0) length = 1;
        bool newline = rune == '\n';
        float advance = newline ? 0.0f :
            font->width(font->userdata, font->height, cursor, length);
        if (newline || (used > 0 && line_width + advance > width)) {
            rows++;
            used = 0;
            line_width = 0.0f;
            if (newline) { cursor += length; continue; }
            if (rows >= max_lines) return rows;
        }
        if (used + (size_t)length < sizeof(line)) {
            memcpy(line + used, cursor, (size_t)length);
            used += (size_t)length;
            line_width += advance;
        }
        cursor += length;
    }
    if (used > 0 && rows < max_lines) rows++;
    return rows;
}

static float item_height(const BongoCatSelfCheckItem *item,
    const struct nk_user_font *title_font, const struct nk_user_font *body_font,
    float width) {
    int rows = measure_text(body_font, width, item->detail, 4);
    return title_font->height + 4.0f + (float)rows * 17.0f + 12.0f;
}

static bool dialog_button(BongoCatApp *app, struct nk_context *context,
    struct nk_command_buffer *canvas, struct nk_rect bounds, const char *label,
    bool primary, BongoCatUIPalette p, float opacity, bool enabled) {
    bool hover = enabled &&
        nk_input_is_mouse_hovering_rect(&context->input, bounds);
    struct nk_color background = primary ? (hover ? p.accent_hover :
        p.accent) : (hover ? p.hover : p.field);
    struct nk_color foreground = primary ? nk_rgb(255, 255, 255) :
        (hover ? p.accent : p.text);
    nk_fill_rect(canvas, bounds, 10, alpha(background, opacity));
    if (!primary) nk_stroke_rect(canvas, bounds, 10, 1,
        alpha(p.border_subtle, opacity));
    draw_centered(canvas, bounds, label, app->preferences->ui.caption_font,
        alpha(foreground, opacity));
    if (hover) bongo_cat_ui_cursor_hover_rect(context, bounds,
        BONGO_CAT_UI_CURSOR_POINTER);
    return hit(context, bounds, enabled);
}

/* 面板内的一行结论: 左侧状态文字 + 标题 + 自动折行的说明 */
static void draw_item(struct nk_command_buffer *canvas,
    const BongoCatSelfCheckItem *item, struct nk_rect row,
    const struct nk_user_font *title_font, const struct nk_user_font *body_font,
    BongoCatUIPalette p, BongoCatApp *app, float opacity) {
    struct nk_color color = status_color_for(item->status, p);
    draw_text(canvas, nk_rect(row.x, row.y + 1, 56, title_font->height),
        status_label(app, item->status), body_font, alpha(color, opacity));
    draw_text(canvas, nk_rect(row.x + 60, row.y,
        row.w - 60, title_font->height), item->title, title_font,
        alpha(p.text, opacity));
    wrap_text(canvas, row.x + 60, row.y + title_font->height + 4.0f,
        row.w - 60, 17.0f, item->detail, body_font, alpha(p.muted, opacity), 4);
}

/* 运行一次自检并打开面板。同步执行 (整体 < 5 秒), 结果直接放报告里。 */
static void selfcheck_open(BongoCatApp *app) {
    if (!app) return;
    memset(&selfcheck.report, 0, sizeof(selfcheck.report));
    selfcheck.app = app;
    selfcheck.open = true;
    selfcheck.input_armed = false;
    selfcheck.opened_ns = SDL_GetTicksNS();
    selfcheck.closing_ns = 0;
    selfcheck.scroll = 0.0f;
    selfcheck.saved = false;
    selfcheck.saved_path[0] = '\0';
    selfcheck.saved_error[0] = '\0';
    selfcheck.feedback[0] = '\0';
    selfcheck.feedback_until_ns = 0;
    bongo_cat_selfcheck_run(app, &selfcheck.report);
    /*
     * 报告是运行时拼出来的文字, 大量汉字不在界面文案里 —— 先把这些字登记进字形图集
     * 并请求重建 (绘制中途会延到帧末), 否则面板里会是一片缺字方块。
     */
    if (app->preferences) {
        bongo_cat_preferences_glyphs_add_text(app->preferences,
            selfcheck.report.text);
        bongo_cat_preferences_reload_fonts(app->preferences);
        app->preferences->render_dirty = true;
    }
}

/* 测试钩子: BONGO_CAT_TEST_SELFCHECK=1 时, 设置窗口第一次可见就自动打开面板。
   与 BONGO_CAT_TEST_PREFS_WINDOW 配合可以截图 / 端到端验证, 不需要合成鼠标点击。 */
static void selfcheck_test_hook(BongoCatPreferences *value) {
    static bool hook_used;
    const char *hook = SDL_getenv("BONGO_CAT_TEST_SELFCHECK");
    if (hook_used || !hook || !hook[0] || !value || !value->visible) return;
    hook_used = true;
    selfcheck_open(value->app);
}

void bongo_cat_preferences_selfcheck_row(BongoCatPreferences *value,
    struct nk_context *context) {
    if (!value || !value->app) return;
    BongoCatApp *app = value->app;
    selfcheck_test_hook(value);
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_RENDERER);
    if (bongo_cat_pref_button(context, "run-selfcheck",
        tr(app, "pages.preference.general.selfcheck.labels.title",
            "Run self-check"),
        tr(app, "pages.preference.general.selfcheck.hints.description",
            "Checks the runtime environment, rendering, Live2D and file "
            "permissions, then produces a report you can copy."),
        tr(app, "pages.preference.general.selfcheck.options.start",
            "Start")))
        selfcheck_open(app);
}

bool bongo_cat_preferences_selfcheck_active(const BongoCatApp *app) {
    return app && selfcheck.open && selfcheck.app == app;
}

void bongo_cat_preferences_selfcheck_close(BongoCatApp *app) {
    if (!bongo_cat_preferences_selfcheck_active(app) || selfcheck.closing_ns)
        return;
    selfcheck.closing_ns = SDL_GetTicksNS();
    if (app->preferences) app->preferences->render_dirty = true;
}

/* 窗口被回收时调用: 面板不跨窗口存在 (报告本身也不留, 下次重新跑即可) */
void bongo_cat_preferences_selfcheck_reset(BongoCatPreferences *value) {
    (void)value;
    memset(&selfcheck, 0, sizeof(selfcheck));
}

static void copy_report(BongoCatApp *app) {
    if (!SDL_SetClipboardText(selfcheck.report.text))
        snprintf(selfcheck.feedback, sizeof(selfcheck.feedback), "%s",
            tr(app, "pages.preference.general.selfcheck.hints.copyFailed",
                "Could not copy the report"));
    else
        snprintf(selfcheck.feedback, sizeof(selfcheck.feedback), "%s",
            tr(app, "pages.preference.general.selfcheck.hints.copied",
                "Report copied to the clipboard"));
    selfcheck.feedback_until_ns = SDL_GetTicksNS() + 2600000000ull;
    app->preferences->render_dirty = true;
}

static void save_report(BongoCatApp *app) {
    char path[BONGO_CAT_PATH_CAP];
    selfcheck.saved = false;
    if (!bongo_cat_selfcheck_default_path(path, sizeof(path))) {
        snprintf(selfcheck.saved_error, sizeof(selfcheck.saved_error), "%s",
            tr(app, "pages.preference.general.selfcheck.hints.saveFailed",
                "Could not save the report"));
        return;
    }
    BongoCatError error = {0};
    if (bongo_cat_selfcheck_save(&selfcheck.report, path, &error)) {
        selfcheck.saved = true;
        snprintf(selfcheck.saved_path, sizeof(selfcheck.saved_path), "%s", path);
        selfcheck.saved_error[0] = '\0';
    } else {
        snprintf(selfcheck.saved_error, sizeof(selfcheck.saved_error), "%s",
            error.message[0] ? error.message :
            tr(app, "pages.preference.general.selfcheck.hints.saveFailed",
                "Could not save the report"));
    }
    app->preferences->render_dirty = true;
}

/* [切换并重启]: 复用既有的后端切换入口 (写配置 -> 落盘 -> 自动重启), 不自己开一条路 */
static void switch_to_opengl(BongoCatApp *app) {
    bongo_cat_app_switch_renderer(app, BONGO_CAT_RENDERER_OPENGL,
        BONGO_CAT_RENDERER_ENTRY_SETTINGS);
    bongo_cat_preferences_selfcheck_close(app);
}

/* 列表区域: 返回本帧消费掉的滚轮是否真的滚了 (决定要不要标脏) */
static bool draw_items(struct nk_context *context,
    struct nk_command_buffer *canvas, struct nk_rect area,
    const struct nk_user_font *title_font, const struct nk_user_font *body_font,
    BongoCatUIPalette p, BongoCatApp *app, float opacity, bool enabled) {
    float content = 0.0f;
    for (size_t i = 0; i < selfcheck.report.count; ++i)
        content += item_height(&selfcheck.report.items[i], title_font,
            body_font, area.w - 60.0f);
    float maximum = NK_MAX(0.0f, content - area.h);
    bool scrolled = false;
    if (enabled) {
        float wheel = context->input.mouse.scroll_delta.y;
        if (wheel != 0.0f &&
            nk_input_is_mouse_hovering_rect(&context->input, area)) {
            selfcheck.scroll -= wheel * 56.0f;
            scrolled = true;
            context->input.mouse.scroll_delta.y = 0;
        }
    }
    selfcheck.scroll = NK_CLAMP(0.0f, selfcheck.scroll, maximum);
    nk_push_scissor(canvas, area);
    float y = area.y - selfcheck.scroll;
    for (size_t i = 0; i < selfcheck.report.count; ++i) {
        const BongoCatSelfCheckItem *item = &selfcheck.report.items[i];
        float height = item_height(item, title_font, body_font, area.w - 60.0f);
        if (y + height >= area.y && y <= area.y + area.h)
            draw_item(canvas, item, nk_rect(area.x, y, area.w, height),
                title_font, body_font, p, app, opacity);
        y += height;
    }
    /* Nuklear 没有 pop: 把裁剪矩形推回整个窗口即为还原 */
    nk_push_scissor(canvas, nk_window_get_bounds(context));
    if (maximum > 0.0f) {
        /* 细滚动条: 只做位置指示, 拖动不是必须的 (滚轮已经够用) */
        struct nk_rect track = nk_rect(area.x + area.w + 8, area.y, 4, area.h);
        nk_fill_rect(canvas, track, 2, alpha(p.border_subtle, opacity));
        float ratio = area.h / content;
        float thumb = NK_MAX(24.0f, area.h * ratio);
        float offset = (area.h - thumb) * (selfcheck.scroll / maximum);
        nk_fill_rect(canvas, nk_rect(track.x, track.y + offset, track.w, thumb),
            2, alpha(p.accent, opacity));
    }
    return scrolled;
}

void bongo_cat_preferences_selfcheck_dialog_draw(BongoCatPreferences *value,
    struct nk_context *context) {
    if (!value || !value->app || !bongo_cat_preferences_selfcheck_active(
            value->app)) return;
    BongoCatApp *app = value->app;
    bongo_cat_ui_cursor_reset(context);
    struct nk_rect region = nk_window_get_bounds(context);
    float width = NK_MIN(660.0f, region.w - 40.0f);
    float height = NK_MIN(500.0f, region.h - 40.0f);
    BongoCatOverlayFrame frame = bongo_cat_preferences_overlay_frame(region,
        width, height, selfcheck.opened_ns, selfcheck.closing_ns);
    if (frame.finished) {
        selfcheck.open = false;
        selfcheck.closing_ns = 0;
        return;
    }
    BongoCatUIPalette p = bongo_cat_ui_palette(bongo_cat_ui_dark(context));
    bongo_cat_preferences_overlay_draw(context, region, &frame, p);
    bool closing = selfcheck.closing_ns != 0;
    float opacity = closing ? frame.visibility : 1.0f;
    struct nk_command_buffer *canvas = nk_window_get_canvas(context);
    nk_fill_rect(canvas, frame.panel, 18, alpha(p.surface, opacity));
    nk_stroke_rect(canvas, frame.panel, 18, 1, alpha(p.border, opacity));
    const struct nk_user_font *title_font = value->ui.label_font;
    const struct nk_user_font *body_font = value->ui.body_font;
    const struct nk_user_font *caption_font = value->ui.caption_font;
    draw_text(canvas, nk_rect(frame.panel.x + 20, frame.panel.y + 18,
        frame.panel.w - 80, title_font->height),
        tr(app, "pages.preference.general.selfcheck.labels.title",
            "Run self-check"), title_font, alpha(p.text, opacity));
    bool enabled = !closing && bongo_cat_preferences_overlay_input_ready(
        context, &selfcheck.input_armed);
    struct nk_rect close_bounds = nk_rect(frame.panel.x + frame.panel.w - 52,
        frame.panel.y + 14, 32, 32);
    bool close = bongo_cat_ui_close_button(context, canvas, close_bounds,
        alpha(p.muted, opacity), alpha(p.accent, opacity), enabled);
    /* 状态摘要: 计数用本地化格式串, 建议行只在真的建议切后端时出现 */
    char summary[256];
    snprintf(summary, sizeof(summary),
        tr(app, "pages.preference.general.selfcheck.status.counts",
            "%u passed · %u warnings · %u failed · %u ms"),
        selfcheck.report.pass, selfcheck.report.warn,
        selfcheck.report.fail, selfcheck.report.elapsed_ms);
    draw_text(canvas, nk_rect(frame.panel.x + 20, frame.panel.y + 44,
        frame.panel.w - 40, body_font->height), summary, body_font,
        alpha(selfcheck.report.fail ? p.danger : p.muted, opacity));
    /* 建议行占一行; 出现时列表整体下移, 否则它会压在计数行上 */
    float list_top = frame.panel.y + 74;
    /* footer 里要放两样: 保存路径/复制反馈一行 + 底部按钮一行 */
    float footer = 96.0f;
    if (selfcheck.report.recommend_opengl) {
        draw_text(canvas, nk_rect(frame.panel.x + 20, frame.panel.y + 64,
            frame.panel.w - 40, body_font->height),
            tr(app, "pages.preference.general.selfcheck.status.vulkanAdvice",
                "Vulkan could not present its first frame; switching to "
                "OpenGL is recommended."), body_font,
            alpha(nk_rgb(0xe8, 0x9c, 0x1c), opacity));
        list_top += 22.0f;
    }
    struct nk_rect area = nk_rect(frame.panel.x + 20, list_top,
        frame.panel.w - 52,
        NK_MAX(80.0f, frame.panel.h - (list_top - frame.panel.y) - footer));
    if (draw_items(context, canvas, area, title_font, body_font, p, app,
        opacity, enabled)) value->render_dirty = true;
    /* 保存结果 / 复制反馈 (面板底部一行, 不依赖浮动提示条) */
    float footer_y = frame.panel.y + frame.panel.h - footer + 4;
    if (selfcheck.saved || selfcheck.saved_error[0] || selfcheck.feedback[0]) {
        char line[384];
        if (selfcheck.saved) {
            snprintf(line, sizeof(line),
                tr(app, "pages.preference.general.selfcheck.status.savedPath",
                    "Saved to: %s"), selfcheck.saved_path);
            draw_text(canvas, nk_rect(frame.panel.x + 20, footer_y,
                frame.panel.w - 40, caption_font->height), line, caption_font,
                alpha(p.muted, opacity));
        } else if (selfcheck.saved_error[0]) {
            draw_text(canvas, nk_rect(frame.panel.x + 20, footer_y,
                frame.panel.w - 40, caption_font->height),
                selfcheck.saved_error, caption_font, alpha(p.danger, opacity));
        } else if (selfcheck.feedback[0] &&
            SDL_GetTicksNS() < selfcheck.feedback_until_ns) {
            draw_text(canvas, nk_rect(frame.panel.x + 20, footer_y,
                frame.panel.w - 40, caption_font->height),
                selfcheck.feedback, caption_font, alpha(p.accent, opacity));
            value->render_dirty = true;
        }
    }
    /* 底部按钮: 右对齐, [切换并重启] 只在建议切后端时出现 */
    float button_y = frame.panel.y + frame.panel.h - 50;
    float right = frame.panel.x + frame.panel.w - 18;
    struct nk_rect switch_bounds = nk_rect(right - 128, button_y, 128, 34);
    struct nk_rect save_bounds = nk_rect(
        (selfcheck.report.recommend_opengl ? switch_bounds.x : right + 8) - 118,
        button_y, 110, 34);
    struct nk_rect copy_bounds = nk_rect(save_bounds.x - 110, button_y, 102, 34);
    if (selfcheck.report.recommend_opengl &&
        dialog_button(app, context, canvas, switch_bounds,
            tr(app, "pages.preference.general.selfcheck.options.switchToOpenGL",
                "Switch & restart"), true, p, opacity, enabled))
        switch_to_opengl(app);
    if (dialog_button(app, context, canvas, copy_bounds,
        tr(app, "pages.preference.general.selfcheck.options.copy",
            "Copy report"), false, p, opacity, enabled))
        copy_report(app);
    if (dialog_button(app, context, canvas, save_bounds,
        tr(app, "pages.preference.general.selfcheck.options.save",
            "Save to file"), false, p, opacity, enabled))
        save_report(app);
    if (close) bongo_cat_preferences_selfcheck_close(app);
    /* 淡入/淡出期间保持出帧, 否则动画会停在半路 */
    if (frame.visibility < 1.0f || closing) value->render_dirty = true;
}
