/*
 * 行为设置对话框 (动作 / 表情 / 声音), 从模型卡片上的那个笑脸图标打开。
 *
 * 结构是三段式: 标题 -> 分段控件 -> 可滚动行列表。
 * 两条设计约束:
 * 1. 分段是否显示由"当前模型真的有没有这一类数据"决定 (tab_available 每次绘制
 * 现算, 不缓存) —— 换个模型, 动作集合就变了, 缓存会显示出一个空标签页;
 * 2. 行列表是虚拟化的 (只画落在视口里的行), 但滚动偏移是按 tab 分别保存的
 * (behavior_scroll[3]), 所以来回切标签页各自的滚动位置都还在。
 * 关闭走的是"先进入淡出 (closing_ns), 淡完 (frame.finished) 才真正清状态",
 * 这段时间里输入一律禁用; input_armed 用来吃掉"打开它的那一次点击"。
 */
#include "preferences_state.h"
#include "preferences_overlay.h"
#include "ui_animation.h"
#include "ui_backend.h"
#include "ui_icons.h"
#include "ui_paint.h"

#include <SDL3/SDL.h>
#include <stdio.h>

static const char *tr(BongoCatPreferences *value, const char *key,
    const char *fallback) {
    return bongo_cat_i18n_get(value->app->i18n, key, fallback);
}

static struct nk_color alpha(struct nk_color color, float amount) {
    return bongo_cat_preferences_overlay_alpha(color, amount);
}
static void text(struct nk_command_buffer *canvas, struct nk_rect bounds,
    const char *value, const struct nk_user_font *font, struct nk_color color) {
    nk_draw_text(canvas, bounds, value, nk_strlen(value), font,
        nk_rgba(0, 0, 0, 0), color);
}
static void centered(struct nk_command_buffer *canvas, struct nk_rect bounds,
    const char *value, const struct nk_user_font *font, struct nk_color color) {
    float width = font->width(font->userdata, font->height, value,
        nk_strlen(value));
    text(canvas, nk_rect(bounds.x + (bounds.w - width) * .5f,
        bounds.y + (bounds.h - font->height) * .5f, width + 1, font->height),
        value, font, color);
}

static bool hit(struct nk_context *context, struct nk_rect bounds, bool enabled) {
    return enabled && nk_input_is_mouse_hovering_rect(&context->input, bounds) &&
        nk_input_is_mouse_click_in_rect(&context->input, NK_BUTTON_LEFT, bounds);
}
/*
 * 某个条目属不属于某个 tab。动作那一档要额外问 live2d 这个 motion 当前可不可见:
 * 行为表是启动时枚举出来的全集, 换模型后会有一批失效项, 不筛掉就会列出点了没反应的行。
 */
static bool matches(const BongoCatPreferences *value,
    const BongoCatBehaviorEntry *entry, int tab) {
    if (tab == 0) return entry->kind == BONGO_CAT_BEHAVIOR_MOTION &&
        bongo_cat_live2d_motion_visible(value->app->live2d,
            entry->group, entry->index);
    if (tab == 1) return entry->kind == BONGO_CAT_BEHAVIOR_EXPRESSION;
    return entry->kind == BONGO_CAT_BEHAVIOR_SOUND && (entry->sound[0] || entry->sound_clear);
}

/* 声音那一档要求 sound[0] 非空: sound_clear 类型的条目本身没有文件名, 列出来也没得改 */
static bool tab_available(const BongoCatPreferences *value, int tab) {
    for (size_t i = 0; i < value->app->behaviors.count; ++i) {
        const BongoCatBehaviorEntry *entry = &value->app->behaviors.entries[i];
        if (matches(value, entry, tab) && (tab != 2 || entry->sound[0])) return true;
    }
    return false;
}

/* 当前 tab 的行数。面板高度是用它算出来的, 所以必须在画面板之前就能拿到 (见 draw) */
static size_t row_count(const BongoCatPreferences *value) {
    if (!tab_available(value, value->behavior_tab)) return 0;
    size_t count = 0;
    for (size_t i = 0; i < value->app->behaviors.count; ++i)
        if (matches(value, &value->app->behaviors.entries[i],
            value->behavior_tab))
            count++;
    return count;
}

bool bongo_cat_preferences_behavior_dialog_active(
    const BongoCatPreferences *value) {
    return value && value->behavior_dialog;
}

void bongo_cat_preferences_behavior_dialog_open(
    BongoCatPreferences *value) {
    if (!value) return;
    SDL_Log("Preferences behavior dialog opened with %llu behaviors",
        (unsigned long long)value->app->behaviors.count);
    value->behavior_dialog = true;
    value->behavior_dialog_input_armed = false;
    value->behavior_dialog_opened_ns = SDL_GetTicksNS();
    value->behavior_dialog_closing_ns = 0;
    value->render_dirty = true;
}

/*
 * 关闭是幂等的 (已在淡出就不重置时间戳); 顺手收掉重命名与快捷键录制这两个会话 ——
 * 它们持有的目标指针属于卡片/页面, 面板关掉之后再留着会写到看不见的地方。
 */
void bongo_cat_preferences_behavior_dialog_close(
    BongoCatPreferences *value) {
    if (!value || !value->behavior_dialog || value->behavior_dialog_closing_ns)
        return;
    value->behavior_dialog_closing_ns = SDL_GetTicksNS();
    bongo_cat_preferences_scrollbar_reset(&value->behavior_scrollbar);
    bongo_cat_preferences_behavior_rename_finish(value, true);
    bongo_cat_preferences_shortcut_cancel(value);
    value->render_dirty = true;
}

static bool draw_header(BongoCatPreferences *value, struct nk_context *context,
    struct nk_command_buffer *canvas, struct nk_rect panel,
    BongoCatUIPalette p, float opacity, bool enabled) {
    text(canvas, nk_rect(panel.x + 20, panel.y + 21, panel.w - 74, 24),
        tr(value, "pages.preference.model.behaviorModal.title",
        "Motions, expressions and audio"), value->ui.label_font, alpha(p.text, opacity));
    struct nk_rect close = nk_rect(panel.x + panel.w - 52, panel.y + 17, 32, 32);
    return bongo_cat_ui_close_button(context, canvas, close,
        alpha(p.muted, opacity), alpha(p.accent, opacity), enabled);
}

/*
 * 分段控件。只画"有数据"的段, 所以每段宽度是按可用段数均分的 —— 用一个固定的
 * 三分之一宽, 只显示两段时右边会空出一条。
 * 切段时要做三件收尾: 重置滚动条 (新列表的高度完全不同)、记下过渡起点、取消正在
 * 进行的快捷键录制 (录到一半切走, 目标行已经不在屏幕上了)。
 */
static void draw_segments(BongoCatPreferences *value,
    struct nk_context *context, struct nk_command_buffer *canvas,
    struct nk_rect panel, BongoCatUIPalette p, float opacity, bool enabled) {
    struct nk_rect wrapper = nk_rect(panel.x + 20, panel.y + 85,
        panel.w - 40, 43);
    nk_fill_rect(canvas, wrapper, 10, alpha(p.field, opacity));
    const char *labels[] = {tr(value,
        "pages.preference.model.behaviorModal.labels.motion", "Motions"), tr(value,
        "pages.preference.model.behaviorModal.labels.expression", "Expressions"), tr(value,
        "pages.preference.model.behaviorModal.labels.audio", "Audio")};
    int count = 0, position = 0;
    for (int i = 0; i < 3; ++i) count += tab_available(value, i);
    if (!count) return;
    float width = (wrapper.w - 6) / count;
    for (int i = 0; i < 3; ++i) {
        if (!tab_available(value, i)) continue;
        struct nk_rect button = nk_rect(wrapper.x + 3 + width * position++,
            wrapper.y + 3, width, 37);
        bool hover = enabled && nk_input_is_mouse_hovering_rect(
            &context->input, button);
        char id[32]; snprintf(id, sizeof(id), "behavior-segment-%d", i);
        float active = bongo_cat_ui_animate_eased(context, id,
            value->behavior_tab == i ? 1.0f : 0.0f, 200,
            BONGO_CAT_UI_EASE_STANDARD);
        struct nk_color background = bongo_cat_ui_color_mix(
            hover ? p.hover : p.field, p.accent, active);
        if (active > .01f && p.effects) bongo_cat_ui_paint_shadow(context,
            button, 8, 0, 2, 10, 0, alpha(nk_rgba(p.accent.r,
            p.accent.g, p.accent.b, 89), opacity * active));
        nk_fill_rect(canvas, button, 8, alpha(background, opacity));
        centered(canvas, button, labels[i], value->ui.caption_font,
            alpha(bongo_cat_ui_color_mix(p.muted,
            nk_rgb(255, 255, 255), active), opacity));
        if (hover) bongo_cat_ui_cursor_hover_rect(context, button,
            BONGO_CAT_UI_CURSOR_POINTER);
        if (hit(context, button, enabled) && value->behavior_tab != i) {
            value->behavior_tab = i;
            bongo_cat_preferences_scrollbar_reset(
                &value->behavior_scrollbar);
            value->behavior_tab_transition_ns = SDL_GetTicksNS();
            bongo_cat_preferences_shortcut_cancel(value);
        }
    }
}

/*
 * 行列表。几处容易出错的地方:
 * - 滚动条画在视口右侧, 行的可用宽度要相应让位 (scrollbar_content_width), 否则
 * 行的右边缘会被滚动条压住;
 * - 只画与视口相交的行, 但 shown 的计数必须对"所有匹配行"递增, 不能只对画出来的
 * 递增, 否则滚动位置会随可见行数漂移;
 * - scissor 用完必须恢复成整个内容区, 忘了恢复会让后面画的东西全被裁掉;
 * - behavior_scroll 的下标是当前 tab, 保存的是"用户停下的偏移", 每帧读回来喂给
 * 滚动条, 变化时再写回并标脏 —— 这就是它跨帧记住位置的方式。
 */
static void draw_rows(BongoCatPreferences *value, struct nk_context *context,
    struct nk_command_buffer *canvas, struct nk_rect panel,
    BongoCatUIPalette p, float opacity, bool enabled, size_t count) {
    struct nk_rect viewport = nk_rect(panel.x + 20, panel.y + 144,
        panel.w - 40, panel.h - 164);
    float content_height = count * 56.0f;
    float maximum = NK_MAX(0.0f, content_height - viewport.h);
    float saved_offset = value->behavior_scroll[value->behavior_tab];
    BongoCatPreferencesScrollbarResult scroll =
        bongo_cat_preferences_scrollbar_draw(context, canvas, viewport,
            content_height, saved_offset, &value->behavior_scrollbar, p.accent,
            opacity, enabled);
    if (scroll.changed) {
        value->behavior_scroll[value->behavior_tab] = scroll.offset;
        value->render_dirty = true;
    }
    float offset = NK_CLAMP(0.0f, scroll.offset, maximum);
    float render_offset = offset;
    float row_width = bongo_cat_preferences_scrollbar_content_width(
        viewport, content_height);
    float content_opacity = opacity;
    if (value->behavior_tab_transition_ns) {
        float progress = (float)(SDL_GetTicksNS() -
            value->behavior_tab_transition_ns) / 180000000.0f;
        if (progress >= 1.0f) value->behavior_tab_transition_ns = 0;
        else {
            float eased = bongo_cat_ui_ease(BONGO_CAT_UI_EASE_SWIFT,
                NK_CLAMP(0.0f, progress, 1.0f));
            content_opacity *= eased;
            render_offset += 5.0f * (1.0f - eased);
            value->render_dirty = true;
        }
    }
    nk_push_scissor(canvas, viewport);
    size_t shown = 0;
    for (size_t i = 0; i < value->app->behaviors.count; ++i) {
        BongoCatBehaviorEntry *entry = &value->app->behaviors.entries[i];
        if (!matches(value, entry, value->behavior_tab)) continue;
        struct nk_rect row = nk_rect(viewport.x,
            viewport.y + shown++ * 56.0f - render_offset, row_width, 56);
        if (row.y + row.h >= viewport.y && row.y <= viewport.y + viewport.h)
            bongo_cat_preferences_behavior_row_draw(value, context, canvas,
                row, entry, p,
                content_opacity, enabled);
    }
    nk_push_scissor(canvas, nk_window_get_content_region(context));
    if (!shown) centered(canvas, viewport, tr(value, "native.noBehaviors",
        "No items"), value->ui.caption_font, alpha(p.muted, content_opacity));
}

/*
 * 面板整体绘制。
 * 进入前先做一次"当前 tab 已失效就换一个"的兜底: 用户可能在面板开着的时候换了
 * 模型, 原来的 tab 变成空的了。
 * 高度由行数算出并夹在窗口内, 所以行多的时候面板更高, 但不会超出窗口。
 * frame.finished 表示淡出播完, 这时才真正把 behavior_dialog 置假 —— 注意不能提前
 * 置假, 否则面板会在还没淡完时突然消失。
 */
void bongo_cat_preferences_behavior_dialog_draw(
    BongoCatPreferences *value, struct nk_context *context) {
    if (!bongo_cat_preferences_behavior_dialog_active(value)) return;
    if (!tab_available(value, value->behavior_tab))
        for (int i = 0; i < 3; ++i) if (tab_available(value, i)) {
            value->behavior_tab = i; break;
        }
    bongo_cat_ui_cursor_reset(context);
    struct nk_rect region = nk_window_get_bounds(context);
    size_t count = row_count(value);
    float width = NK_MIN(540.0f, region.w - 48.0f);
    float height = NK_MIN(165.0f + count * 56.0f, region.h - 48.0f);
    BongoCatOverlayFrame frame = bongo_cat_preferences_overlay_frame(
        region, width, height, value->behavior_dialog_opened_ns,
        value->behavior_dialog_closing_ns);
    if (frame.finished) {
        value->behavior_dialog = false;
        value->behavior_dialog_opened_ns = value->behavior_dialog_closing_ns = 0;
        return;
    }
    BongoCatUIPalette p = bongo_cat_ui_palette(bongo_cat_ui_dark(context));
    bongo_cat_preferences_overlay_draw(context, region, &frame, p);
    bool closing = value->behavior_dialog_closing_ns != 0;
    bool input_ready = bongo_cat_preferences_overlay_input_ready(context,
        &value->behavior_dialog_input_armed);
    float opacity = closing ? frame.visibility : 1.0f;
    struct nk_command_buffer *canvas = nk_window_get_canvas(context);
    nk_fill_rect(canvas, frame.panel, 18, alpha(p.surface, opacity));
    nk_stroke_rect(canvas, frame.panel, 18, 1, alpha(p.border, opacity));
    bool close = draw_header(value, context, canvas, frame.panel, p,
        opacity, !closing && input_ready);
    draw_segments(value, context, canvas, frame.panel, p, opacity,
        !closing && input_ready);
    draw_rows(value, context, canvas, frame.panel, p, opacity,
        !closing && input_ready, count);
    bool outside = hit(context, region, !closing && input_ready) &&
        !nk_input_is_mouse_hovering_rect(&context->input, frame.panel);
    if (close || outside) bongo_cat_preferences_behavior_dialog_close(value);
    if (frame.visibility < 1.0f || closing) value->render_dirty = true;
}
