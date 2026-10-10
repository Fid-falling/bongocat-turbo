/*
 * 设置窗口里的浮动提示条 (toast): 导入结果、删除成功/失败、缓存目录已更新等。
 *
 * 状态是 preferences 里的一个固定 4 槽环形池 (notices[4]), 不是队列也不是动态分配:
 * 提示可能从工作线程收尾、目录刷新回调等任何地方触发, 用固定槽位可以保证不分配、
 * 不会失败。
 * 槽位选择规则 (notice_show): 优先用"空槽或已过期槽"; 都满时**抢占最旧的那条** ——
 * 连发多条提示时, 用户最该看到的是最新的, 而不是让新的排队等到天荒地老。
 * 绘制期间会顺手清理过期槽 (active_notices), 所以提示的生命周期不需要额外的定时器。
 * 每条提示自带时间戳, 显示顺序按时间戳升序 (而不是槽位顺序), 否则环形池的复用会
 * 让提示上下乱跳。
 */
#include "preferences_notice.h"
#include "preferences_state.h"
#include "ui_animation.h"
#include "ui_backend.h"
#include "ui_catime.h"
#include "ui_paint.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

/* 显示时长 2.6s, 进入动画 250ms; 这两个值配合"最多同时 4 条 + 每条 51px 行距"使用 */
enum { NOTICE_DURATION_MS = 2600, NOTICE_ENTER_MS = 250 };

/* 挑槽位: 空槽/过期槽优先, 全满则抢占 started_ns 最早的那条 */
void bongo_cat_preferences_notice_show(BongoCatApp *app,
    const char *message, bool error) {
    if (!app || !app->preferences || !message || !message[0]) return;
    BongoCatPreferences *value = app->preferences;
    uint64_t now = SDL_GetTicksNS();
    BongoCatPreferenceNotice *target = &value->notices[0];
    for (size_t i = 0; i < sizeof(value->notices) / sizeof(value->notices[0]); ++i) {
        BongoCatPreferenceNotice *notice = &value->notices[i];
        if (!notice->message[0] || notice->until_ns <= now) {
            target = notice; break;
        }
        if (notice->started_ns < target->started_ns) target = notice;
    }
    snprintf(target->message, sizeof(target->message), "%s", message);
    target->error = error;
    target->started_ns = now;
    target->until_ns = now + NOTICE_DURATION_MS * 1000000ULL;
    value->render_dirty = true;
}

/*
 * 收集当前有效的提示并按时间排序 (插入排序: 最多 4 个元素, 不值得上 qsort)。
 * 这里同时负责"过期即回收": 把过期槽清零, 于是下一条提示 show 时就能复用这个位置,
 * 不需要任何后台清理逻辑。
 */
static size_t active_notices(BongoCatPreferences *value, uint64_t now,
    BongoCatPreferenceNotice **items) {
    size_t count = 0;
    for (size_t i = 0; i < sizeof(value->notices) / sizeof(value->notices[0]); ++i) {
        BongoCatPreferenceNotice *notice = &value->notices[i];
        if (notice->message[0] && notice->until_ns <= now)
            memset(notice, 0, sizeof(*notice));
        if (notice->message[0]) items[count++] = notice;
    }
    for (size_t i = 1; i < count; ++i) {
        BongoCatPreferenceNotice *item = items[i];
        size_t j = i;
        while (j && items[j - 1]->started_ns > item->started_ns) {
            items[j] = items[j - 1]; j--;
        }
        items[j] = item;
    }
    return count;
}

/*
 * 截断过长的提示文字并返回最终宽度。
 * 逐个回退 prefix 而不是按字符数估算: 多字节字符必须整字截掉, 而且末尾还要给 "..."
 * 留位置, 否则会出现半个汉字或省略号被裁掉。
 * 提示条不换行 (toast 多行会很丑), 所以宁可截断。
 */
static float fit_notice_text(const struct nk_user_font *font,
    const char *source, float maximum, char *output, size_t capacity) {
    snprintf(output, capacity, "%s", source);
    float width = font->width(font->userdata, font->height,
        output, nk_strlen(output));
    if (width <= maximum) return width;
    size_t prefix = strlen(source);
    if (prefix > capacity - 4) prefix = capacity - 4;
    while (prefix) {
        prefix--;
        while (prefix && ((unsigned char)source[prefix] & 0xc0) == 0x80)
            prefix--;
        memcpy(output, source, prefix);
        memcpy(output + prefix, "...", 4);
        width = font->width(font->userdata, font->height,
            output, nk_strlen(output));
        if (width <= maximum) return width;
    }
    snprintf(output, capacity, "...");
    return font->width(font->userdata, font->height, output, 3);
}

/*
 * 一条提示: 从下方 12px 处弹上来 (SPRING 缓动) + 宽度从 90% 展开。
 * 悬停不会暂停计时 —— 提示条不可交互, 做成"可悬停暂停"只会让用户以为能点。
 * 这里无条件标脏: 提示在显示期间必须持续要帧, 否则动画会停在半路 (这正是
 * needs_frame 之外的另一条"主动拉帧"来源)。
 */
static void draw_notice(BongoCatPreferences *value,
    struct nk_context *context, BongoCatPreferenceNotice *notice,
    float width, float y, uint64_t now) {
    BongoCatUIPalette p = bongo_cat_ui_palette(
        bongo_cat_ui_dark(context));
    const struct nk_user_font *font = bongo_cat_ui_label_font(context);
    char label_text[sizeof(notice->message)];
    float text_width = fit_notice_text(font, notice->message,
        NK_MAX(40.0f, width - 100.0f), label_text, sizeof(label_text));
    float toast_width = NK_MIN(text_width + 36.0f, width - 64.0f);
    float elapsed = (float)(now - notice->started_ns) /
        (NOTICE_ENTER_MS * 1000000.0f);
    float progress = bongo_cat_ui_ease(BONGO_CAT_UI_EASE_SPRING,
        NK_CLAMP(0.0f, elapsed, 1.0f));
    float opacity = NK_CLAMP(0.0f, progress, 1.0f);
    float shown_width = toast_width * (.9f + .1f * progress);
    struct nk_rect bounds = nk_rect((width - shown_width) * .5f,
        y - 12.0f * (1.0f - progress), shown_width, 41.0f);
    struct nk_color tone = notice->error ? p.danger : p.pink;
    if (p.effects) bongo_cat_ui_paint_shadow(context, bounds, 20, 0, 8,
        notice->error ? 25.0f : 22.0f, 0, nk_rgba(tone.r, tone.g, tone.b,
        (nk_byte)((notice->error ? 102 : 89) * opacity)));
    struct nk_command_buffer *canvas = nk_window_get_canvas(context);
    nk_fill_rect(canvas, bounds, 20, nk_rgba(tone.r, tone.g, tone.b,
        (nk_byte)(255 * opacity)));
    struct nk_rect label = nk_rect(bounds.x + (bounds.w - text_width) * .5f,
        bounds.y + (bounds.h - font->height) * .5f,
        NK_MIN(text_width + 1, bounds.w - 20), font->height);
    nk_draw_text(canvas, label, label_text, nk_strlen(label_text), font,
        nk_rgba(0, 0, 0, 0), nk_rgba(255, 255, 255, (nk_byte)(255 * opacity)));
    value->render_dirty = true;
}

/* 提示条画在设置窗口的最上层 (在所有页面与对话框之后调用), 从上往下堆叠, 行距 51px */
void bongo_cat_preferences_notice_draw(BongoCatPreferences *value,
    struct nk_context *context, float width, float height) {
    (void)height;
    if (!value) return;
    uint64_t now = SDL_GetTicksNS();
    BongoCatPreferenceNotice *items[4];
    size_t count = active_notices(value, now, items);
    for (size_t i = 0; i < count; ++i)
        draw_notice(value, context, items[i], width, 20.0f + i * 51.0f, now);
}
