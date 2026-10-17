/*
 * 设置 → 互动 → [音效] 区: 总音量 / 全部静音 / 当前模型的逐条音效开关。
 *
 * 为什么放在互动页: 这一页回答的是"桌宠怎么响应我的操作" (输入 / 镜像 / 权限),
 * 而音效正是"桌宠什么时候出声"的另一半; 放在窗口页 (位置/样式/背景) 或性能页都不
 * 贴题。音效清单跟随当前定焦模型 (app->behaviors 就是当前模型的行为目录), 换模型
 * 之后这一区自动变成新模型的清单。
 *
 * 清单只列"真的会出声"的条目: kind=sound 且带文件 (sound-clear 是停止按钮, 不列),
 * 以及带 Sound 的动作。同一个音频文件被多条行为引用时只列一次 —— 用户要关的是
 * "这个声音", 不是"这条绑定"。
 *
 * 逐条开关走 pending 机制 (与模型卡片上的参与开关同一套): 设置窗口一帧会画两次,
 * 在点击当帧直接改 settings 会被第二次重放翻回去。翻一次的动作在
 * bongo_cat_preferences_process_sound_toggle 里做。
 */
#include "preferences_state.h"
#include "preferences_theme.h"
#include "preferences_widgets.h"
#include "preferences_controls.h"
#include "preferences_icons.h"
#include "model_sound.h"
#include "ui_backend.h"
#include "ui_catime.h"
#include "ui_icons.h"
#include "bongo_cat/audio.h"
#include "bongo_cat/i18n.h"
#include "bongo_cat/path.h"
#include "bongo_cat/sound.h"

#include <stdio.h>
#include <string.h>

static const char *tr(BongoCatApp *app, const char *key, const char *fallback) {
    return bongo_cat_i18n_get(app->i18n, key, fallback);
}

static void section_gap(struct nk_context *context, float pixels) {
    context->current->layout->at_y += pixels;
}

/* 与播放路径同一口径: sound 条目要有文件, 动作要带绑定音效。 */
static bool playable(const BongoCatBehaviorEntry *entry) {
    if (!entry->sound[0]) return false;
    if (entry->kind == BONGO_CAT_BEHAVIOR_SOUND) return !entry->sound_clear;
    return entry->kind == BONGO_CAT_BEHAVIOR_MOTION;
}

static bool duplicate(const BongoCatApp *app, size_t index) {
    for (size_t i = 0; i < index; ++i)
        if (playable(&app->behaviors.entries[i]) &&
            !strcmp(app->behaviors.entries[i].sound,
                app->behaviors.entries[index].sound))
            return true;
    return false;
}

/* 去掉模型目录前缀之后的名字, 只用来在行里做副标题 (帮用户认出是哪个文件)。 */
static const char *sound_file_label(const BongoCatApp *app, const char *path,
    char *buffer, size_t capacity) {
    const char *model_id = app->loaded_model;
    const BongoCatModelEntry *model = model_id[0]
        ? bongo_cat_models_find(&app->models, model_id) : NULL;
    const char *directory = model ? model->adapter_directory : NULL;
    if (!directory || !directory[0]) directory = model ? model->directory : NULL;
    const char *relative = path;
    if (directory && directory[0]) {
        size_t length = strlen(directory);
        if (!strncmp(path, directory, length) &&
            (path[length] == '/' || path[length] == '\\'))
            relative = path + length + 1;
    }
    if (!relative[0]) relative = bongo_cat_path_name(path);
    snprintf(buffer, capacity, "%s", relative);
    return buffer;
}

/* 一行音效: 名称 + 文件名副标题 + 右侧开关。禁用态整行变灰, 但开关本身仍可点。 */
static void draw_sound_row(BongoCatPreferences *value,
    struct nk_context *context, size_t index,
    const BongoCatBehaviorEntry *entry, const char *id, bool disabled) {
    BongoCatApp *app = value->app;
    nk_layout_row_dynamic(context, 52, 1);
    struct nk_rect row;
    if (nk_widget(&row, context) == NK_WIDGET_INVALID) return;
    BongoCatUIPalette p = bongo_cat_ui_palette(bongo_cat_ui_dark(context));
    struct nk_command_buffer *canvas = nk_window_get_canvas(context);
    nk_fill_rect(canvas, row, 10, p.surface);
    nk_stroke_rect(canvas, row, 10, 1, p.border_subtle);
    bongo_cat_pref_icon_draw(canvas, nk_rect(row.x + 14, row.y + 16, 20, 20),
        BONGO_CAT_PREF_ICON_RANDOM_SOUND, disabled ? p.muted : p.accent);
    char file[BONGO_CAT_PATH_CAP];
    const char *label = entry->label[0] ? entry->label :
        bongo_cat_path_name(entry->sound);
    const struct nk_user_font *label_font = bongo_cat_ui_label_font(context);
    const struct nk_user_font *caption_font = bongo_cat_ui_caption_font(context);
    float text_width = row.w - 130.0f;
    nk_draw_text(canvas, nk_rect(row.x + 44,
        row.y + 8, NK_MAX(40.0f, text_width), label_font->height), label,
        nk_strlen(label), label_font, nk_rgba(0, 0, 0, 0),
        disabled ? p.muted : p.text);
    nk_draw_text(canvas, nk_rect(row.x + 44,
        row.y + 29, NK_MAX(40.0f, text_width), caption_font->height),
        sound_file_label(app, entry->sound, file, sizeof(file)),
        nk_strlen(file), caption_font, nk_rgba(0, 0, 0, 0), p.muted);
    /* 开关复用偏好页的动画开关; id 必须逐行唯一且跨帧稳定, 用清单序号即可
       (清单顺序来自模型文件, 同一模型内不会变)。 */
    char key[48];
    snprintf(key, sizeof(key), "sound-toggle-%zu", index);
    bool enabled = !disabled;
    struct nk_rect cell = nk_rect(row.x + row.w - 72, row.y + 12, 64, 28);
    if (bongo_cat_pref_control_toggle_rect(context, key, &enabled, cell, true)) {
        snprintf(value->pending_sound_id, sizeof(value->pending_sound_id),
            "%s", id);
        value->pending_sound_toggle = true;
        value->render_dirty = true;
    }
}

/* "该模型没有音效"的空状态: 一行居中的说明文字, 不留一个空槽。 */
static void draw_empty(BongoCatPreferences *value,
    struct nk_context *context) {
    BongoCatUIPalette p = bongo_cat_ui_palette(bongo_cat_ui_dark(context));
    nk_layout_row_dynamic(context, 56, 1);
    struct nk_rect row;
    if (nk_widget(&row, context) == NK_WIDGET_INVALID) return;
    struct nk_command_buffer *canvas = nk_window_get_canvas(context);
    nk_fill_rect(canvas, row, 10, p.surface);
    nk_stroke_rect(canvas, row, 10, 1, p.border_subtle);
    const char *message = tr(value->app,
        "pages.preference.cat.labels.soundEmpty",
        "This model has no sounds.");
    nk_draw_text(canvas, nk_rect(row.x + 16,
        row.y + (row.h - value->ui.caption_font->height) * .5f,
        row.w - 32, value->ui.caption_font->height), message,
        nk_strlen(message), value->ui.caption_font, nk_rgba(0, 0, 0, 0),
        p.muted);
}

/* [音效] 区正文。 */
void bongo_cat_preferences_sound_section(BongoCatPreferences *value,
    struct nk_context *context) {
    BongoCatApp *app = value->app;
    BongoCatSoundPreferences *sound = &app->settings.sound;
    if (!bongo_cat_preferences_options_begin(value, context,
        BONGO_CAT_OPTIONS_SOUND,
        tr(app, "pages.preference.cat.labels.soundSection", "Sound"),
        BONGO_CAT_PREF_ICON_RANDOM_SOUND))
        return;
    float volume = (float)bongo_cat_settings_sound_volume(&app->settings);
    if (bongo_cat_pref_slider(context, "sound-volume", tr(app,
            "pages.preference.cat.labels.soundVolume", "Volume"),
            tr(app, "pages.preference.cat.labels.soundVolumeHint",
                "Applies to every sound this app plays."),
            0.0f, &volume, 100.0f, 1.0f,
            (float)BONGO_CAT_SOUND_VOLUME_DEFAULT)) {
        sound->volume = (int)volume;
        if (sound->volume < 0) sound->volume = 0;
        if (sound->volume > 100) sound->volume = 100;
        /* 立即作用到正在播的声音 (引擎级增益), 不用等下一次触发。 */
        if (app->audio)
            bongo_cat_audio_set_volume(app->audio,
                (float)sound->volume / 100.0f);
        value->render_dirty = true;
    }
    bool muted = sound->muted != 0;
    if (bongo_cat_pref_toggle(context, "sound-mute-all", tr(app,
            "pages.preference.cat.labels.soundMuteAll", "Mute all sounds"),
            tr(app, "pages.preference.cat.labels.soundMuteAllHint",
                "Keeps the per-sound switches below; turn this off to restore them."),
            &muted)) {
        sound->muted = muted ? 1 : 0;
        /* 打开总闸时把正在响的也停掉: 只拦下一次播放会让长音效继续放完,
           用户的感受就是"点了静音还在响"。 */
        if (sound->muted && app->audio) bongo_cat_audio_stop(app->audio);
        value->render_dirty = true;
    }
    bongo_cat_pref_status(context, "sound-list-heading", tr(app,
        "pages.preference.cat.labels.soundList", "Sounds in this model"),
        tr(app, "pages.preference.cat.labels.soundListHint",
            "Turn off a sound you do not want to hear. Changes apply immediately."));
    size_t index = 0;
    for (size_t i = 0; i < app->behaviors.count; ++i) {
        const BongoCatBehaviorEntry *entry = &app->behaviors.entries[i];
        if (!playable(entry) || duplicate(app, i)) continue;
        char id[BONGO_CAT_SOUND_ID_CAP];
        if (!bongo_cat_app_sound_id(app, entry->sound, id, sizeof(id))) {
            /* 连 id 都拼不出来 (路径异常) 时仍然列出来并允许关掉: 用文件名兜底,
               否则用户永远看不到、也关不掉这条顽固音效。 */
            snprintf(id, sizeof(id), "%s:%s", app->loaded_model[0] ?
                app->loaded_model : "model", bongo_cat_path_name(entry->sound));
        }
        draw_sound_row(value, context, index++, entry, id,
            bongo_cat_settings_sound_disabled(&app->settings, id));
    }
    if (!index) draw_empty(value, context);
    section_gap(context, 4);
    bongo_cat_preferences_options_end(value, context);
}

/* 帧末处理逐项开关: settings 只在两次渲染之间翻转一次 (见文件头)。 */
void bongo_cat_preferences_process_sound_toggle(BongoCatPreferences *value) {
    if (!value || !value->pending_sound_toggle) return;
    char id[BONGO_CAT_SOUND_ID_CAP];
    snprintf(id, sizeof(id), "%s", value->pending_sound_id);
    value->pending_sound_toggle = false;
    value->pending_sound_id[0] = '\0';
    bool disabled = bongo_cat_settings_sound_disabled(&value->app->settings, id);
    bongo_cat_settings_set_sound_disabled(&value->app->settings, id, !disabled);
    /* 刚被关掉的那条如果正在播就立刻停: 与"全部静音"同一个理由 —— 只拦下一次
       播放会让用户觉得开关没生效。 */
    if (!disabled && value->app->audio) {
        for (size_t i = 0; i < value->app->behaviors.count; ++i) {
            const BongoCatBehaviorEntry *entry = &value->app->behaviors.entries[i];
            char entry_id[BONGO_CAT_SOUND_ID_CAP];
            if (!entry->sound[0] ||
                !bongo_cat_app_sound_id(value->app, entry->sound, entry_id,
                    sizeof(entry_id))) continue;
            if (!strcmp(entry_id, id)) {
                bongo_cat_audio_stop_path(value->app->audio, entry->sound);
                break;
            }
        }
    }
    value->render_dirty = true;
}
