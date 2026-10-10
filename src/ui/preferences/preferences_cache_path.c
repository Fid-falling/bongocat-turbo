/*
 * "缓存目录"这一行的完整实现: 文本框 + 浏览 (原生文件夹对话框) + 重置。
 *
 * 这一行踩过的坑比看上去多, 改动前务必看清三条契约:
 * 1. 原生对话框是异步的, 结果通过自定义 SDL 用户事件回到主循环, 而且必须走
 * preferences.c 里 dialog_event 这条路 (在按窗口 ID 过滤之前处理), 否则
 * 事件没有窗口 ID 会被丢掉, 症状就是"选完文件夹毫无反应";
 * 2. 事件里的路径字符串由生产者 SDL_strdup, 由消费者 (dialog_event) SDL_free,
 * 中间跨线程, 谁都不能省掉这一步;
 * 3. cache_root 一改就要立刻调 bongo_cat_storage_paths_apply_cache 让缓存目录
 * 生效, 改完只存设置不生效的话用户会以为没保存。
 */
#include "preferences_state.h"
#include "preferences_notice.h"
#include "preferences_text_session.h"
#include "preferences_text_edit.h"
#include "preferences_widgets.h"
#include "preferences_widgets_internal.h"
#include "bongo_cat/i18n.h"
#include "bongo_cat/path.h"
#include "storage_paths.h"
#ifdef _WIN32
#include "windows_dialog.h"
#endif

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

/* Custom event used to ferry a chosen folder from the async dialog thread back
   to the main loop. The path string is duplicated by the callback and freed by
   the consumer. */
static Uint32 cache_folder_event_type = 0;

typedef struct BongoCatCacheBrowseContext {
    BongoCatApp *app;
    Uint32 event_type;
} BongoCatCacheBrowseContext;

/*
 * 把路径搬回主线程。必须 SDL_strdup: files[] 只在回调期间有效, SDL 回调返回后就会
 * 释放, 直接存指针会让事件带着一个悬空指针飘到主循环。
 * data2 存 app 指针, 用于消费端确认这个事件是不是发给自己的 (进程里可能同时存在
 * 多个 app 实例, 比如冒烟测试)。
 */
static void queue_folder_result(BongoCatApp *app, Uint32 type,
    const char *path) {
    if (!type) return;
    SDL_Event event = {0};
    event.type = type;
    event.user.data1 = path ? SDL_strdup(path) : NULL;
    event.user.data2 = app;
    SDL_PushEvent(&event);
}

/* 回调里负责释放自己的 ctx: SDL 不管这块内存, 漏掉就是每次浏览泄漏一次 */
static void folder_callback(void *userdata, const char *const *files,
    int filter) {
    (void)filter;
    BongoCatCacheBrowseContext *ctx = userdata;
    if (!ctx) return;
    const char *path = files && files[0] ? files[0] : NULL;
    queue_folder_result(ctx->app, ctx->event_type, path);
    SDL_free(ctx);
}

static const char *tr(BongoCatApp *app, const char *key, const char *fallback) {
    return bongo_cat_i18n_get(app->i18n, key, fallback);
}

static bool cache_path_active(const BongoCatPreferences *value) {
    return value &&
        bongo_cat_preferences_text_session_active(&value->cache_path);
}

/*
 * 结束编辑并落盘。changed 要在写回之前算出来, 否则永远为假; 只有真的变了才去
 * apply_cache 并弹提示 —— 反复点同一路径不该刷出一串"已更新"。
 */
static bool cache_path_save(BongoCatPreferences *value) {
    if (!value) return false;
    BongoCatPreferencesTextSession *session = &value->cache_path;
    if (!bongo_cat_preferences_text_session_active(session)) return false;
    bongo_cat_text_edit_trim(session->text);
    bool changed = strcmp(session->text, value->app->settings.cache_root) != 0;
    snprintf(value->app->settings.cache_root,
        sizeof(value->app->settings.cache_root), "%s", session->text);
    bongo_cat_preferences_text_session_reset(session);
    SDL_StopTextInput(value->window);
    value->render_dirty = true;
    if (changed) {
        BongoCatError error = {0};
        if (bongo_cat_storage_paths_apply_cache(value->app, &error) != false) {
            bongo_cat_preferences_notice_show(value->app, tr(value->app,
                "pages.preference.general.hints.cacheApplied",
                "Cache directory updated"), false);
        } else {
            bongo_cat_preferences_notice_show(value->app, error.message, true);
        }
    }
    return true;
}

void bongo_cat_preferences_cache_path_draw(BongoCatPreferences *value,
    struct nk_context *context) {
    if (!value || !value->app) return;
    /*
     * 测试钩子: BONGO_CAT_TEST_CACHE_DIALOG=<目录> 会在这一帧模拟"用户点了浏览并
     * 选中该目录"。它走的是和原生文件夹对话框完全相同的事件通道 (SDL 用户事件 ->
     * 主循环 -> preferences.c 里的 dialog_event), 所以能覆盖"选完没反应"这类问题。
     * 放在函数最前面: 这一行滚出可见区域时下面的 form 会直接返回, 钩子就跑不到了。
     */
    {
        static bool hook_used;
        const char *hook = SDL_getenv("BONGO_CAT_TEST_CACHE_DIALOG");
        if (!hook_used && hook && hook[0]) {
            hook_used = true;
            if (!cache_folder_event_type)
                cache_folder_event_type = SDL_RegisterEvents(1);
            if (cache_folder_event_type != (Uint32)-1)
                queue_folder_result(value->app, cache_folder_event_type, hook);
        }
    }
    const char *label = tr(value->app,
        "pages.preference.general.labels.cacheDirectory", "Cache directory");
    const char *detail = tr(value->app,
        "pages.preference.general.hints.cacheDirectory",
        "Folder used for asset and model caches. Leave empty for the default.");
    BongoCatUIPalette p = bongo_cat_ui_palette(bongo_cat_ui_dark(context));
    const struct nk_user_font *font = bongo_cat_ui_label_font(context);
    FormStyle saved;
    int lines = bongo_cat_pref_detail_lines(context, detail);
    if (!bongo_cat_pref_form_begin(context, "cache-path", lines + 1, &saved))
        return;
    bongo_cat_pref_form_label(context, label);
    nk_layout_row_dynamic(context, 36, 1);
    struct nk_rect row;
    nk_widget(&row, context);

    bool editing = cache_path_active(value);
    const char *shown = editing ? value->cache_path.text :
        value->app->settings.cache_root;
    if (!shown[0]) shown = tr(value->app,
        "pages.preference.general.hints.defaultCache", "Default (automatic)");
    const char *browse_text = tr(value->app,
        "pages.preference.general.options.browse", "Browse");
    const char *clear_text = tr(value->app,
        "pages.preference.general.options.clear", "Reset");
    float pad_w = 12, gap = 6, min_w = 56;
    float browse_w = font->width(font->userdata, font->height,
        browse_text, nk_strlen(browse_text)) + pad_w * 2;
    float clear_w = font->width(font->userdata, font->height,
        clear_text, nk_strlen(clear_text)) + pad_w * 2;
    if (browse_w < min_w) browse_w = min_w;
    if (clear_w < min_w) clear_w = min_w;
    float buttons_w = browse_w + gap + clear_w;
    float field_w = row.w - buttons_w - 8;
    if (field_w < 80) field_w = 80;
    struct nk_rect field = nk_rect(row.x, row.y, field_w, row.h);
    struct nk_rect browse = nk_rect(row.x + field_w + 8, row.y,
        browse_w, row.h);
    struct nk_rect clear = nk_rect(browse.x + browse.w + gap, row.y,
        clear_w, row.h);

    bool hover = nk_input_is_mouse_hovering_rect(&context->input, field);
    bool clicked = hover && nk_input_is_mouse_click_in_rect(
        &context->input, NK_BUTTON_LEFT, field);
    if (clicked && !editing) {
        bongo_cat_preferences_text_session_begin(&value->cache_path,
            "cache-path", value->app->settings.cache_root, field);
        SDL_StartTextInput(value->window);
        value->render_dirty = true;
    }
    nk_fill_rect(nk_window_get_canvas(context), field, 8,
        editing ? p.hover_pink : p.field);
    nk_stroke_rect(nk_window_get_canvas(context), field, 8, 1,
        editing ? p.pink : p.border_subtle);
    struct nk_rect text = nk_rect(field.x + 8, field.y +
        (field.h - font->height) * .5f, field.w - 16, font->height);
    nk_draw_text(nk_window_get_canvas(context), text, shown, nk_strlen(shown),
        font, nk_rgba(0, 0, 0, 0), editing ? p.pink : p.text);
    if (editing) {
        float caret = font->width(font->userdata, font->height,
            shown, (int)value->cache_path.cursor);
        caret = NK_MIN(caret, field.w - 16);
        nk_stroke_line(nk_window_get_canvas(context),
            field.x + 8 + caret, field.y + 4,
            field.x + 8 + caret, field.y + field.h - 4, 1, p.pink);
    }
    if (hover || editing)
        bongo_cat_ui_cursor_hover_rect(context, field,
            BONGO_CAT_UI_CURSOR_TEXT);

    bool browse_hover = nk_input_is_mouse_hovering_rect(&context->input, browse);
    nk_fill_rect(nk_window_get_canvas(context), browse, 8,
        browse_hover ? p.hover : p.field);
    nk_stroke_rect(nk_window_get_canvas(context), browse, 8, 1,
        browse_hover ? p.accent : p.border_subtle);
    float bw = NK_MIN(browse.w, font->width(font->userdata, font->height,
        browse_text, nk_strlen(browse_text)));
    struct nk_rect btext = nk_rect(browse.x + (browse.w - bw) * .5f,
        browse.y + (browse.h - font->height) * .5f, bw, font->height);
    nk_draw_text(nk_window_get_canvas(context), btext, browse_text,
        nk_strlen(browse_text), font, nk_rgba(0, 0, 0, 0), p.accent);
    if (nk_input_is_mouse_click_in_rect(&context->input, NK_BUTTON_LEFT,
        browse)) {
        if (editing) cache_path_save(value);
        bongo_cat_preferences_cache_path_browse(value);
        value->render_dirty = true;
    }
    if (browse_hover)
        bongo_cat_ui_cursor_hover_rect(context, browse,
            BONGO_CAT_UI_CURSOR_POINTER);

    bool clear_hover = nk_input_is_mouse_hovering_rect(&context->input, clear);
    nk_fill_rect(nk_window_get_canvas(context), clear, 8,
        clear_hover ? p.hover : p.field);
    nk_stroke_rect(nk_window_get_canvas(context), clear, 8, 1,
        clear_hover ? p.accent : p.border_subtle);
    float cw = NK_MIN(clear.w, font->width(font->userdata, font->height,
        clear_text, nk_strlen(clear_text)));
    struct nk_rect ctext = nk_rect(clear.x + (clear.w - cw) * .5f,
        clear.y + (clear.h - font->height) * .5f, cw, font->height);
    nk_draw_text(nk_window_get_canvas(context), ctext, clear_text,
        nk_strlen(clear_text), font, nk_rgba(0, 0, 0, 0), p.text);
    if (nk_input_is_mouse_click_in_rect(&context->input, NK_BUTTON_LEFT,
        clear)) {
        if (editing) bongo_cat_preferences_text_session_reset(
            &value->cache_path);
        SDL_StopTextInput(value->window);
        if (value->app->settings.cache_root[0]) {
            snprintf(value->app->settings.cache_root,
                sizeof(value->app->settings.cache_root), "%s", "");
            bongo_cat_storage_paths_apply_cache(value->app, NULL);
            bongo_cat_preferences_notice_show(value->app, tr(value->app,
                "pages.preference.general.hints.cacheReset",
                "Cache directory reset to default"), false);
        }
        value->render_dirty = true;
    }
    if (clear_hover)
        bongo_cat_ui_cursor_hover_rect(context, clear,
            BONGO_CAT_UI_CURSOR_POINTER);

    bongo_cat_pref_description(context, detail, lines);
    bongo_cat_pref_form_end(context, &saved);
}

/*
 * 消费异步对话框的结果。两处容易漏:
 * - 只处理 data2 == 本 app 的事件, 但仍然要无条件 SDL_free(path) —— 事件是我们
 * 自己排出来的, 不管认不认这个 app, 这份字符串都由这条路径负责收尸;
 * - 无论用户是确认还是取消都要重画 (取消也要弹一条提示)。
 * 返回 true 表示事件已消费: 调用方是 preferences.c 里"按窗口过滤"之前的那一段。
 */
bool bongo_cat_preferences_cache_path_dialog_event(BongoCatPreferences *value,
    const SDL_Event *event) {
    if (!value || !event || !cache_folder_event_type ||
        event->type != cache_folder_event_type) return false;
    const char *path = (const char *)event->user.data1;
    if (event->user.data2 == value->app) {
        if (path) {
            snprintf(value->app->settings.cache_root,
                sizeof(value->app->settings.cache_root), "%s", path);
            BongoCatError error = {0};
            if (bongo_cat_storage_paths_apply_cache(value->app, &error))
                bongo_cat_preferences_notice_show(value->app, tr(value->app,
                    "pages.preference.general.hints.cacheApplied",
                    "Cache directory updated"), false);
            else
                bongo_cat_preferences_notice_show(value->app,
                    error.message, true);
        } else {
            bongo_cat_preferences_notice_show(value->app, tr(value->app,
                "pages.preference.general.hints.cacheCancelled",
                "Cache directory selection cancelled"), false);
        }
    }
    SDL_free((void *)path);
    value->render_dirty = true;
    return true;
}

bool bongo_cat_preferences_cache_path_event(BongoCatPreferences *value,
    const SDL_Event *event) {
    if (!value || !event) return false;
    if (bongo_cat_preferences_text_session_event(&value->cache_path, event,
        value->ui.layout_scale, value->ui.label_font, 0.0f).handled) {
        value->render_dirty = true;
        return true;
    }
    return false;
}

/*
 * 打开文件夹选择框。事件类型只注册一次 (SDL_RegisterEvents 每次调用都会占用一个
 * 类型号, 每帧注册会把号段吃光); 注册失败返回 (Uint32)-1, 这时宁可什么都不做。
 * Windows 走自己的 windows_dialog 实现 (需要 owner 窗口才能做成模态), 其它平台
 * 用 SDL 自带的。
 */
void bongo_cat_preferences_cache_path_browse(BongoCatPreferences *value) {
    if (!value || !value->window) return;
    if (!cache_folder_event_type)
        cache_folder_event_type = SDL_RegisterEvents(1);
    if (cache_folder_event_type == (Uint32)-1) return;
    BongoCatCacheBrowseContext *ctx = SDL_calloc(1, sizeof(*ctx));
    if (!ctx) return;
    ctx->app = value->app;
    ctx->event_type = cache_folder_event_type;
#ifdef _WIN32
    bongo_cat_windows_show_open_folder_dialog(folder_callback, ctx,
        value->window, value->app->settings.cache_root, false);
#else
    SDL_ShowOpenFolderDialog(folder_callback, ctx, value->window,
        value->app->settings.cache_root, false);
#endif
}

/* ESC / 关窗时放弃这次编辑: 只丢会话内容, 不碰 settings.cache_root */
void bongo_cat_preferences_cache_path_cancel(BongoCatPreferences *value) {
    if (!value) return;
    if (bongo_cat_preferences_text_session_active(&value->cache_path)) {
        bongo_cat_preferences_text_session_reset(&value->cache_path);
        SDL_StopTextInput(value->window);
        value->render_dirty = true;
    }
}
