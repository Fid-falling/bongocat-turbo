/*
 * 关于页底部信息条: 版本号 + 更新按钮 + (失败时的)查看发布版本 + 反馈链接 + 打开日志目录。
 *
 * 三条是本文件的核心逻辑:
 * 1. 布局是"能并排就并排, 放不下就换两行"(stacked)。所有宽度都是先把各段量出来
 * 再累加得到的, 加一项就要把它算进 total/actions_width, 否则窄窗口下会被裁掉;
 * 2. 更新按钮是一个多态按钮, 文案和点击行为都由 update_snapshot 的 status 决定
 * (检查中/下载中 xx%/校验中/准备重启/有新版/商店版/不支持自更新/普通检查)。文案由
 * update_label 统一给出, 状态判断在这里重复了一次 —— 改动状态语义时两处要一起改。
 * 3. "查看发布版本"只在 ERROR 时出现: 检查失败后按钮只会重试, 得给用户一条不经过
 * GitHub API 的出路 (限流时 API 打不通, 发布页本身仍能打开)。
 * 版本号用 BONGO_CAT_VERSION_DISPLAY (带构建渠道信息), 不要再拼别的字符串。
 *
 * 一键更新的"下载中 -> 校验中 -> 准备重启"三段都体现在这个按钮上: 点击即开始, 百分比
 * 直接写在按钮文案里; 当前用的源、以及失败原因由 preferences_update_status.c 的状态条
 * 显示 (用户可能已经切到别的设置页, 那时按钮根本看不见)。
 * 三段进行中都不接受点击 —— 重复点击不会让下载更快, 只会让用户以为点漏了。
 */
#include "preferences_about_footer.h"
#include "preferences_notice.h"
#include "preferences_state.h"
#include "ui_animation.h"
#include "ui_backend.h"
#include "ui_catime.h"
#include "ui_icons.h"
#include "ui_paint.h"
#include "update_service.h"

#include <SDL3/SDL.h>
#include <stdio.h>

/*
 * 4/5 缩放的说明: 日志链接要比其它链接小一号, 但 Nuklear 的字体是不可变的,
 * 所以这里复制一份 caption_font 改高度, 存进 state (不能是局部变量: 这一帧里
 * Nuklear 还持有这个指针)。
 */
#define SUPPORT_LOGS_SCALE (4.0f / 5.0f)
#define SUPPORT_LOGS_ICON_SIZE (14.0f * SUPPORT_LOGS_SCALE)
#define SUPPORT_LOGS_GAP 4.0f

static const char *tr(BongoCatPreferences *value, const char *key,
    const char *fallback) {
    return bongo_cat_i18n_get(value->app->i18n, key, fallback);
}

static float width(const struct nk_user_font *font, const char *value) {
    return font->width(font->userdata, font->height, value, nk_strlen(value));
}

/* 每次都按当前 caption_font 重算, 因为字体图集重建后原来的指针会失效 */
static const struct nk_user_font *logs_font(BongoCatPreferences *value) {
    value->support_logs_font = *value->ui.caption_font;
    value->support_logs_font.height =
        value->ui.caption_font->height * SUPPORT_LOGS_SCALE;
    return &value->support_logs_font;
}

static float logs_content_width(const struct nk_user_font *font,
    const char *label) {
    return SUPPORT_LOGS_ICON_SIZE + SUPPORT_LOGS_GAP + width(font, label);
}

static void centered(struct nk_command_buffer *canvas, struct nk_rect bounds,
    const char *value, const struct nk_user_font *font, struct nk_color color) {
    float text_width = width(font, value);
    nk_draw_text(canvas, nk_rect(bounds.x + (bounds.w - text_width) * .5f,
        bounds.y + (bounds.h - font->height) * .5f,
        NK_MIN(text_width + 1.0f, bounds.w), font->height), value,
        nk_strlen(value), font, nk_rgba(0, 0, 0, 0), color);
}

static bool hit(struct nk_context *context, struct nk_rect bounds) {
    return nk_input_is_mouse_hovering_rect(&context->input, bounds) &&
        nk_input_is_mouse_click_in_rect(&context->input, NK_BUTTON_LEFT, bounds);
}

static void link_cursor(struct nk_context *context, struct nk_rect bounds) {
    if (nk_input_is_mouse_hovering_rect(&context->input, bounds))
        bongo_cat_ui_cursor_hover_rect(context, bounds,
            BONGO_CAT_UI_CURSOR_POINTER);
}

static struct nk_color link_color(struct nk_context *context,
    struct nk_rect bounds, const char *animation_id, BongoCatUIPalette p) {
    bool hover = nk_input_is_mouse_hovering_rect(&context->input, bounds);
    float amount = bongo_cat_ui_animate_eased(context, animation_id,
        hover ? 1.0f : 0.0f, 200, BONGO_CAT_UI_EASE_STANDARD);
    return bongo_cat_ui_color_mix(p.accent, p.pink, amount);
}

/* 打开日志目录失败要明确提示: 这一项是排障入口, 点了没反应会让用户以为程序坏了 */
static void open_logs(BongoCatPreferences *value) {
    if (bongo_cat_platform_open_directory(value->app->log_root)) return;
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
        "Cannot open logs directory: %s", value->app->log_root);
    bongo_cat_preferences_notice_show(value->app, tr(value,
        "native.support.openLogsFailed", "Unable to open logs folder"), true);
}

/*
 * 打开更新/发布页面, 失败时统一提示。两处入口(更新按钮、"查看发布版本"链接)共用:
 * 带上 SDL_GetError 是因为打开失败多半是系统拒绝了 URL, 只说"无法打开"没法排查。
 */
static void open_update_page(BongoCatPreferences *value) {
    if (bongo_cat_update_open(value->app->update)) return;
    char detail[384];
    const char *reason = SDL_GetError();
    snprintf(detail, sizeof(detail), "%s: %s", tr(value,
        "native.support.openUpdateFailed", "Unable to open the update page"),
        reason && reason[0] ? reason : "system rejected the URL");
    bongo_cat_preferences_notice_show(value->app, detail, true);
}

/*
 * 更新按钮。右上角那个小红点是"真有可下载的新版本"的标记 —— 它和按钮文案是两套
 * 判断: 文案可能只写"检查更新", 但有红点就说明点下去会去下载。
 * 点击语义: 进行中的阶段 (检查/下载/校验/准备重启) 直接返回, 不发第二次请求;
 * 有新版或上次失败但有版本号时, 走一键更新入口 (Windows 上它就是"下载并替换");
 * 商店/不支持自更新的平台打开商店或发布页; 其余情况发起一次普通的手动检查。
 */
/* 一键更新是否正在进行 (界面据此禁用按钮、并让状态条保持可见)。 */
static bool update_busy(BongoCatUpdateStatus status) {
    return status == BONGO_CAT_UPDATE_CHECKING ||
        status == BONGO_CAT_UPDATE_DOWNLOADING ||
        status == BONGO_CAT_UPDATE_VERIFYING ||
        status == BONGO_CAT_UPDATE_INSTALLING;
}

static void draw_update(BongoCatPreferences *value,
    struct nk_context *context, struct nk_command_buffer *canvas,
    struct nk_rect bounds, const char *label, float content_width,
    BongoCatUIPalette p, const BongoCatUpdateSnapshot *snapshot) {
    if (p.effects) bongo_cat_ui_paint_shadow(context, bounds, 10,
        0, 4, 14, 0, nk_rgba(p.accent.r, p.accent.g, p.accent.b, 89));
    nk_fill_rect(canvas, bounds, 10, p.accent);
    if (snapshot->status == BONGO_CAT_UPDATE_AVAILABLE ||
            snapshot->status == BONGO_CAT_UPDATE_DOWNLOADING ||
            (snapshot->status == BONGO_CAT_UPDATE_ERROR &&
                snapshot->release.version[0])) {
        /* Mark the actionable update without obscuring its label. */
        nk_fill_circle(canvas, nk_rect(bounds.x + bounds.w - 9.0f,
            bounds.y + 5.0f, 6.0f, 6.0f), p.danger);
    }
    float content_x = bounds.x + (bounds.w - content_width) * .5f;
    bongo_cat_preferences_icon_draw(value, canvas, BONGO_CAT_UI_ICON_SYNC,
        nk_rect(content_x, bounds.y + 10, 16, 16), nk_rgb(255, 255, 255));
    centered(canvas, nk_rect(content_x + 24, bounds.y,
        content_width - 23, bounds.h), label, value->ui.caption_font,
        nk_rgb(255, 255, 255));
    link_cursor(context, bounds);
    if (hit(context, bounds)) {
        if (update_busy(snapshot->status)) return;
        if (snapshot->status == BONGO_CAT_UPDATE_AVAILABLE ||
                (snapshot->status == BONGO_CAT_UPDATE_ERROR &&
                    snapshot->release.version[0])) {
            if (!bongo_cat_update_start(value->app->update))
                bongo_cat_preferences_notice_show(value->app, tr(value,
                    "native.support.updateFailed",
                    "Unable to check for updates"), true);
        } else if (snapshot->status == BONGO_CAT_UPDATE_STORE ||
            snapshot->status == BONGO_CAT_UPDATE_UNSUPPORTED) {
            open_update_page(value);
        } else if (!bongo_cat_update_check(value->app->update, true)) {
            bongo_cat_preferences_notice_show(value->app, tr(value,
                "native.support.updateFailed",
                "Unable to check for updates"), true);
        } else value->render_dirty = true;
    }
}

/*
 * 检查失败时才出现的"查看发布版本"链接 (见 update_label 之外的这条分支)。
 * 失败时更新按钮只会重新发起请求, 而限流恰恰是 GitHub API 打不通的时候 ——
 * 这条链接走的是发布页本身, 不经过 API, 让用户至少有一条查到最新版本的路。
 * 与 update_label 里 UNSUPPORTED 的按钮文案不同: 那是整条通道不可用, 这里是"这次没查成"。
 */
static void draw_releases(BongoCatPreferences *value,
    struct nk_context *context, struct nk_command_buffer *canvas,
    struct nk_rect bounds, const char *label, BongoCatUIPalette p) {
    struct nk_color color = link_color(context, bounds,
        "support-releases-hover", p);
    centered(canvas, bounds, label, value->ui.caption_font, color);
    link_cursor(context, bounds);
    if (hit(context, bounds)) open_update_page(value);
}

/* 更新按钮的文案表。buffer 由调用方提供, 因为"更新到 vX.Y.Z"与"下载中 xx%"都要拼接。
   进行中的三段共用这里: 下载显示百分比 (服务端没给长度时退化成不带百分比的"下载中"), 
   校验与准备重启各一句短语 —— 所有文案都不含状态码或英文原文。 */
static const char *update_label(BongoCatPreferences *value,
    const BongoCatUpdateSnapshot *snapshot, char *buffer, size_t capacity) {
    switch (snapshot->status) {
    case BONGO_CAT_UPDATE_CHECKING:
        return tr(value, "native.support.checkingUpdate", "Checking...");
    case BONGO_CAT_UPDATE_DOWNLOADING: {
        unsigned percent = snapshot->total
            ? (unsigned)((snapshot->downloaded * 100) / snapshot->total) : 0;
        if (!snapshot->total)
            return tr(value, "native.support.updateDownloadingUnknown",
                "Downloading...");
        snprintf(buffer, capacity, tr(value, "native.support.updateDownloading",
            "Downloading %u%%"), percent);
        return buffer;
    }
    case BONGO_CAT_UPDATE_VERIFYING:
        return tr(value, "native.support.updateVerifying", "Verifying...");
    case BONGO_CAT_UPDATE_INSTALLING:
        return tr(value, "native.support.updateInstalling", "Restarting...");
    case BONGO_CAT_UPDATE_AVAILABLE:
        snprintf(buffer, capacity, tr(value,
            "native.support.downloadUpdate", "Update to v%s"),
            snapshot->release.version);
        return buffer;
    case BONGO_CAT_UPDATE_ERROR:
        if (snapshot->release.version[0]) {
            snprintf(buffer, capacity, tr(value,
                "native.support.downloadUpdate", "Update to v%s"),
                snapshot->release.version);
            return buffer;
        }
        return tr(value, "native.support.checkUpdate", "Check for updates");
    case BONGO_CAT_UPDATE_STORE:
        return tr(value, "native.support.openStore", "Open Microsoft Store");
    case BONGO_CAT_UPDATE_UNSUPPORTED:
        return tr(value, "native.support.viewReleases", "View releases");
    default:
        return tr(value, "native.support.checkUpdate", "Check for updates");
    }
}

/* 反馈链接与"打开日志": 日志那一项用小字体加一个文件夹图标, 中间那条竖线是分隔符 */
static void draw_links(BongoCatPreferences *value,
    struct nk_context *context, struct nk_command_buffer *canvas,
    struct nk_rect feedback, struct nk_rect logs, const char *feedback_label,
    const char *logs_label, BongoCatUIPalette p) {
    const struct nk_user_font *small_font = logs_font(value);
    struct nk_color feedback_color = link_color(context, feedback,
        "support-feedback-hover", p);
    centered(canvas, feedback, feedback_label, value->ui.caption_font,
        feedback_color);
    link_cursor(context, feedback);
    if (hit(context, feedback) && !SDL_OpenURL(
            "https://github.com/vladelaina/BongoCat/issues"))
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Cannot open URL: %s", SDL_GetError());

    nk_stroke_line(canvas, logs.x - 10, logs.y + 11,
        logs.x - 10, logs.y + logs.h - 11, 1, p.border_subtle);
    struct nk_color logs_color = link_color(context, logs,
        "support-logs-hover", p);
    float content_width = logs_content_width(small_font, logs_label);
    float content_x = logs.x + (logs.w - content_width) * .5f;
    bongo_cat_preferences_icon_draw(value, canvas, BONGO_CAT_UI_ICON_FOLDER,
        nk_rect(content_x, logs.y + (logs.h - SUPPORT_LOGS_ICON_SIZE) * .5f,
        SUPPORT_LOGS_ICON_SIZE, SUPPORT_LOGS_ICON_SIZE), logs_color);
    centered(canvas, nk_rect(content_x + SUPPORT_LOGS_ICON_SIZE +
        SUPPORT_LOGS_GAP, logs.y, width(small_font, logs_label) + 1,
        logs.h), logs_label, small_font, logs_color);
    link_cursor(context, logs);
    if (hit(context, logs)) open_logs(value);
}

/*
 * 底部信息条。所有子宽度先量后排, 最后用一个 total 判断要不要折行:
 * info 段 = "App version" + 版本号
 * actions 段 = 更新按钮 + (只有失败时的)查看发布版本 + 反馈 + 日志
 * 折行时两段各自居中, 并让 info 行上移 8px / actions 行下移 22px —— 这两个偏移量
 * 是配合 34px 行高调出来的, 改行高要一起改。
 */
void bongo_cat_preferences_about_footer(BongoCatPreferences *value,
    struct nk_context *context, struct nk_command_buffer *canvas,
    struct nk_rect bounds, BongoCatUIPalette p) {
    const char *version_label = tr(value,
        "native.support.version", "App version");
    BongoCatUpdateSnapshot update_snapshot;
    bongo_cat_update_snapshot(value->app->update, &update_snapshot);
    char update_buffer[96];
    const char *update = update_label(value, &update_snapshot,
        update_buffer, sizeof(update_buffer));
    const char *feedback = tr(value, "native.support.feedback", "Feedback");
    const char *logs = tr(value, "native.support.openLogs", "Open logs");
    const char *releases = tr(value, "native.support.viewReleases",
        "View releases");
    const struct nk_user_font *small_font = logs_font(value);
    const char *version = BONGO_CAT_VERSION_DISPLAY;
    /* 只有检查失败(限流、网络不通等)才多出这一个入口, 所以宽度也要一起算进
       actions_width, 否则窄窗口下它会被右边裁掉。 */
    bool show_releases = update_snapshot.status == BONGO_CAT_UPDATE_ERROR;
    float label_width = width(value->ui.caption_font, version_label) + 2;
    float version_width = width(value->ui.label_font, version) + 2;
    float update_text_width = width(value->ui.caption_font, update);
    float update_content_width = 24.0f + update_text_width;
    float update_width = NK_MAX(110.0f, update_content_width + 28.0f);
    float feedback_width = NK_MAX(56.0f,
        width(value->ui.caption_font, feedback) + 4.0f);
    float logs_width = logs_content_width(small_font, logs) + 4.0f;
    float releases_width = show_releases ? NK_MAX(56.0f,
        width(value->ui.caption_font, releases) + 4.0f) : 0.0f;
    float links_width = feedback_width + 20.0f + logs_width;
    float actions_width = update_width + 14.0f + links_width +
        (show_releases ? releases_width + 14.0f : 0.0f);
    float info_width = label_width + version_width + 8.0f;
    float total = info_width + actions_width + 14.0f;
    bool stacked = total > bounds.w;
    float info_x = bounds.x + (bounds.w -
        (stacked ? info_width : total)) * .5f;
    float info_y = bounds.y + (stacked ? -8.0f : 0.0f);
    centered(canvas, nk_rect(info_x, info_y, label_width, 36), version_label,
        value->ui.caption_font, p.muted);
    centered(canvas, nk_rect(info_x + label_width + 8, info_y,
        version_width, 36), version, value->ui.label_font, p.text);

    float actions_x = stacked ? bounds.x +
        (bounds.w - actions_width) * .5f : info_x + info_width + 14.0f;
    float actions_y = bounds.y + (stacked ? 22.0f : 0.0f);
    struct nk_rect update_button = nk_rect(actions_x, actions_y,
        update_width, 36);
    draw_update(value, context, canvas, update_button, update,
        update_content_width, p, &update_snapshot);
    float link_x = actions_x + update_width + 14;
    if (show_releases) {
        draw_releases(value, context, canvas, nk_rect(link_x, actions_y,
            releases_width, 36), releases, p);
        link_x += releases_width + 14;
    }
    struct nk_rect feedback_link = nk_rect(link_x,
        actions_y, feedback_width, 36);
    struct nk_rect logs_link = nk_rect(feedback_link.x + feedback_width + 20,
        actions_y, logs_width, 36);
    draw_links(value, context, canvas, feedback_link, logs_link,
        feedback, logs, p);
}
