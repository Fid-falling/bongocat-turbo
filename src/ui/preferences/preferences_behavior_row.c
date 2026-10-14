/*
 * 行为设置对话框里的一行: 左名(可重命名) + 中快捷键录制 + 右试听/停止。
 *
 * 与 bongo_cat_preferences_behavior_dialog.c 的契约: 行只负责画和响应, 行高固定
 * 56 像素 (对话框用 "行数 x 56" 算面板高度和滚动量), 改成别的值两边都要改。
 *
 * 重命名的状态存在 value->behavior_rename 这个会话里, 靠 session->id 与 entry->id
 * 比对判断"这一行是不是正在被改名" —— 所以同一时间只可能有一行在编辑。
 * 编辑中还每帧回写 session->bounds: 对话框是可滚动的, 行会移动, 不跟着更新的话
 * 光标和点击范围会留在旧位置。
 */
#include "preferences_state.h"
#include "preferences_controls.h"
#include "bongo_cat/audio.h"
#include "bongo_cat/shortcut.h"
#include "preferences_overlay.h"
#include "preferences_shortcut_clear.h"
#include "ui_animation.h"
#include "ui_backend.h"
#include "ui_icons.h"
#include "ui_paint.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static struct nk_color alpha(struct nk_color color, float amount) {
    return bongo_cat_preferences_overlay_alpha(color, amount);
}

static void text(struct nk_command_buffer *canvas, struct nk_rect bounds,
    const char *value, const struct nk_user_font *font, struct nk_color color) {
    nk_draw_text(canvas, bounds, value, nk_strlen(value), font,
        nk_rgba(0, 0, 0, 0), color);
}

static bool hit(struct nk_context *context, struct nk_rect bounds, bool enabled) {
    return enabled && nk_input_is_mouse_hovering_rect(&context->input, bounds) &&
        nk_input_is_mouse_click_in_rect(&context->input, NK_BUTTON_LEFT, bounds);
}

/*
 * 取(或按需创建)某个行为对应的快捷键绑定记录。
 * 惰性创建: 行为是启动时批量枚举的, 用户可能一辈子都不给某个行为设快捷键, 预先
 * 全建出来既占配置空间又会让配置文件里塞满空条目。
 * 表满返回 NULL, 调用方据此把这一行的快捷键编辑器整个跳过。
 */
static BongoCatBehaviorShortcut *binding_for(BongoCatSettings *config,
    const char *id) {
    for (size_t i = 0; i < config->behavior_shortcut_count; ++i)
        if (!strcmp(config->behavior_shortcuts[i].id, id))
            return &config->behavior_shortcuts[i];
    if (config->behavior_shortcut_count >= BONGO_CAT_BEHAVIOR_BINDING_CAP) return NULL;
    BongoCatBehaviorShortcut *binding =
        &config->behavior_shortcuts[config->behavior_shortcut_count++];
    memset(binding, 0, sizeof(*binding));
    snprintf(binding->id, sizeof(binding->id), "%s", id);
    return binding;
}

/*
 * 显示名优先级: 用户改过的名字 > "停止所有声音"这种特殊语义 > 模型里的原始名。
 * 特殊条目必须在改名之前判定, 否则它会被显示成一个模型内部名 (用户看不懂)。
 */
static const char *display_label(BongoCatPreferences *value, const BongoCatBehaviorEntry *entry,
    const BongoCatBehaviorShortcut *binding) {
    if (binding && binding->label[0])
        return binding->label;
    if (entry->sound_clear) return bongo_cat_i18n_get(value->app->i18n,
        "pages.preference.model.behaviorModal.labels.stopAllAudio", "Stop all audio");
    return entry->label;
}

/*
 * 快捷键编辑器 (行中间那块)。
 * 录制中要持续标脏: 呼吸光晕是时间驱动的, 不主动要帧它就不会继续动。
 * "清除"按钮只在真的录了东西时出现 (按钮占掉右侧 25px, 文本宽度要相应让位),
 * 清掉后同时把 shortcut_disabled 置真 —— 只清字符串不置禁用位, 会被当成"用空
 * 快捷键启用", 下帧又被恢复成默认值。
 */
static bool shortcut_editor(BongoCatPreferences *value,
    struct nk_context *context, struct nk_command_buffer *canvas,
    struct nk_rect bounds, const char *id, BongoCatBehaviorShortcut *shortcut,
    BongoCatUIPalette p, float opacity, bool enabled) {
    bool active = bongo_cat_preferences_shortcut_active(value, id);
    bool hover = enabled && nk_input_is_mouse_hovering_rect(&context->input, bounds);
    if (active && p.effects) {
        float phase = (float)(SDL_GetTicksNS() % 1500000000ULL) / 1500000000.0f;
        float pulse = sinf(phase * 6.2831853f);
        bongo_cat_ui_paint_shadow(context, bounds, 10, 0, 0,
            5 + 3 * pulse, 0, alpha(nk_rgba(p.pink.r, p.pink.g,
            p.pink.b, 76), opacity));
        value->render_dirty = true;
    }
    nk_fill_rect(canvas, bounds, 10, alpha(active ? p.hover_pink :
        (hover ? p.hover : p.field), opacity));
    nk_stroke_rect(canvas, bounds, 10, 1, alpha(active ? p.pink :
        (hover ? p.accent : p.border_subtle), opacity));
    char shortcut_label[BONGO_CAT_SHORTCUT_CAP * 2];
    bongo_cat_shortcut_format(shortcut->shortcut, shortcut_label, sizeof(shortcut_label));
    const char *shown = active ? bongo_cat_i18n_get(value->app->i18n,
        "components.shortcut.hints.pressRecordShortcut", "Press shortcut") :
        (shortcut->shortcut[0] ? shortcut_label :
        bongo_cat_i18n_get(value->app->i18n,
        "components.shortcut.hints.clickRecordShortcut",
        "Click to record shortcut"));
    bool show_keyboard = active || !shortcut->shortcut[0];
    float text_x = bounds.x + (show_keyboard ? 39.0f : 13.0f);
    if (show_keyboard) bongo_cat_preferences_icon_draw(value, canvas,
        BONGO_CAT_UI_ICON_KEYBOARD, nk_rect(bounds.x + 13,
        bounds.y + 9, 18, 18), alpha(active ? p.pink : p.muted, opacity));
    text(canvas, nk_rect(text_x, bounds.y + 8,
        bounds.x + bounds.w - text_x -
        (shortcut->shortcut[0] && !active ? 31.0f : 9.0f), 21), shown,
        value->ui.caption_font,
        alpha(active ? p.pink : (hover ? p.accent : p.muted), opacity));
    struct nk_rect clear = nk_rect(bounds.x + bounds.w - 25,
        bounds.y + 8, 17, 20);
    if (bongo_cat_pref_shortcut_clear(context, canvas, id, clear, p,
        opacity, enabled && !active && shortcut->shortcut[0])) {
        shortcut->shortcut[0] = '\0';
        shortcut->shortcut_disabled = true;
        value->render_dirty = true; return false;
    }
    if (hover) bongo_cat_ui_cursor_hover_rect(context, bounds,
        BONGO_CAT_UI_CURSOR_POINTER);
    return hit(context, bounds, enabled);
}

/*
 * 画名字区域, 并处理"点一下开始改名"。
 * 重命名会话是按 id 认领的, 只有 id 相同的那一行才进编辑态并画光标; 其它行照常
 * 显示标签。选中态 (select_all) 下不画光标 —— 此时整段被高亮, 再画一根竖线反而怪。
 */
static void draw_name(BongoCatPreferences *value, struct nk_context *context,
    struct nk_command_buffer *canvas, struct nk_rect bounds,
    BongoCatBehaviorEntry *entry, BongoCatBehaviorShortcut *binding,
    BongoCatUIPalette p, float opacity, bool enabled) {
    const char *label = display_label(value, entry, binding);
    BongoCatPreferencesTextSession *session = &value->behavior_rename;
    bool renaming = !strcmp(session->id, entry->id);
    bool hover = enabled && nk_input_is_mouse_hovering_rect(
        &context->input, bounds);
    if (renaming || hover) {
        nk_fill_rect(canvas, bounds, 8, alpha(renaming ? p.hover_pink : p.hover,
            opacity));
        nk_stroke_rect(canvas, bounds, 8, 1,
            alpha(renaming ? p.pink : p.accent, opacity));
    }
    const char *shown = renaming ? session->text : label;
    struct nk_rect text_bounds = nk_rect(bounds.x + 8,
        bounds.y + 9, bounds.w - 16, 21);
    if (renaming && session->select_all)
        nk_fill_rect(canvas, text_bounds, 4,
            alpha(p.selection, opacity));
    text(canvas, text_bounds, shown, value->ui.caption_font,
        alpha(p.text, opacity));
    if (renaming) {
        session->bounds = bounds;
        float caret = value->ui.caption_font->width(
            value->ui.caption_font->userdata, value->ui.caption_font->height,
            shown, (int)session->cursor);
        caret = NK_MIN(caret, bounds.w - 18);
        if (!session->select_all)
            nk_stroke_line(canvas, bounds.x + 8 + caret, bounds.y + 8,
                bounds.x + 8 + caret, bounds.y + bounds.h - 8, 1,
                alpha(p.pink, opacity));
    } else if (hover) {
        bongo_cat_ui_cursor_hover_rect(context, bounds, BONGO_CAT_UI_CURSOR_TEXT);
        if (hit(context, bounds, enabled) && binding)
            bongo_cat_preferences_behavior_rename_begin(value, entry, binding,
                bounds);
    }
}

/*
 * 一行的布局是从右往左定的: 先钉住最右边的播放键, 再往左排快捷键框, 剩下的才是
 * 名字区 —— 名字可以被压缩, 但那两个控件不能。
 * 播放键是"试听/停止"二态: 同一个声音正在播就停, 否则执行这个行为。
 * 快捷键框的 id 要带 entry->id 前缀 (behavior-xxx), 它是动画与录制会话的键, 用
 * 固定名字会让所有行共用一次录制。
 */
/*
 * 一行的布局是从右往左定的: 先钉住最右边的播放键, 再往左排快捷键框, 剩下的才是
 * 名字区 —— 名字可以被压缩, 但那两个控件不能。
 * 播放键是"试听/停止"二态: 同一个声音正在播就停, 否则执行这个行为。
 * 快捷键框的 id 要带 entry->id 前缀 (behavior-xxx), 它是动画与录制会话的键, 用
 * 固定名字会让所有行共用一次录制。
 */
void bongo_cat_preferences_behavior_row_draw(BongoCatPreferences *value,
    struct nk_context *context, struct nk_command_buffer *canvas,
    struct nk_rect row, BongoCatBehaviorEntry *entry, BongoCatUIPalette p,
    float opacity, bool enabled) {
    struct nk_rect play;
    struct nk_rect shortcut_bounds;
    bool participating = true;
    bool has_sw = entry->kind == BONGO_CAT_BEHAVIOR_MOTION ||
        entry->kind == BONGO_CAT_BEHAVIOR_EXPRESSION;
    if (has_sw) {
        participating = !bongo_cat_settings_random_disabled(
            &value->app->settings, entry->id);
        /* 开关复用偏好页的动画开关, 右对齐到行尾(试听键右侧), 播放键左移让位。 */
        play = nk_rect(row.x + row.w - 110, row.y + 10, 36, 36);
    } else {
        play = nk_rect(row.x + row.w - 52, row.y + 10, 36, 36);
    }
    shortcut_bounds = nk_rect(play.x - 188, row.y + 10, 180, 36);
    struct nk_rect name = nk_rect(row.x + 8, row.y + 9,
        NK_MAX(48.0f, shortcut_bounds.x - row.x - 16), 38);
    if (has_sw) {
        struct nk_rect sw_cell = nk_rect(row.x + row.w - 56, row.y - 6,
            56, 68);
        if (bongo_cat_pref_control_toggle_rect(context, entry->id,
            &participating, sw_cell, enabled)) {
            /* 只登记 pending, 由 process_behavior_toggle 在两次渲染之间翻转一次,
               避免嵌套渲染重放把这次点击再翻回去。 */
            snprintf(value->pending_behavior_id,
                sizeof(value->pending_behavior_id), "%s", entry->id);
            value->pending_behavior_toggle = true;
            value->render_dirty = true;
        }
    }
    BongoCatBehaviorShortcut *binding = binding_for(&value->app->settings,
        entry->id);
    draw_name(value, context, canvas, name, entry, binding, p, opacity, enabled);
    bool play_enabled = enabled;
    bool play_hover = play_enabled && nk_input_is_mouse_hovering_rect(
        &context->input, play);
    nk_fill_rect(canvas, play, 10, alpha(play_hover ? p.hover : p.field, opacity));
    nk_stroke_rect(canvas, play, 10, 1, alpha(play_hover ? p.accent :
        p.border_subtle, opacity));
    bool playing = entry->kind == BONGO_CAT_BEHAVIOR_SOUND &&
        bongo_cat_audio_is_playing(value->app->audio, entry->sound);
    if (playing)
        nk_fill_rect(canvas, nk_rect(play.x + 12, play.y + 12, 12, 12), 2,
            alpha(!play_enabled ? p.muted : play_hover ? p.accent : p.text, opacity));
    else bongo_cat_preferences_icon_draw(value, canvas, BONGO_CAT_UI_ICON_PLAY,
        nk_rect(play.x + 10, play.y + 10, 16, 16),
        alpha(!play_enabled ? p.muted : play_hover ? p.accent : p.text, opacity));
    if (play_hover) bongo_cat_ui_cursor_hover_rect(context, play,
        BONGO_CAT_UI_CURSOR_POINTER);
    if (hit(context, play, play_enabled)) {
        if (playing) bongo_cat_audio_stop_path(value->app->audio, entry->sound);
        else bongo_cat_app_run_behavior(value->app, entry);
        value->render_dirty = true;
    }
    if (!binding) return;
    char id[BONGO_CAT_ID_CAP + 16];
    snprintf(id, sizeof(id), "behavior-%.*s", (int)sizeof(id) - 10, entry->id);
    if (shortcut_editor(value, context, canvas, shortcut_bounds, id, binding,
        p, opacity, enabled))
        bongo_cat_preferences_shortcut_begin(value, id, binding->shortcut,
            sizeof(binding->shortcut));
}