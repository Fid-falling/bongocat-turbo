/*
 * 音效策略的运行时实现 (见 model_sound.h)。
 *
 * 三层判定顺序是刻意的:
 *   1. "全部静音" 最高优先级 —— 它是用户按下的总闸, 逐项开关只是子开关;
 *   2. 逐项禁用 (settings.sound.disabled) 命中就跳过;
 *   3. 允许, 并把总音量作为增益下发。
 *
 * 日志只走 DEBUG: 生产版默认日志级别是 WARN, 静音时每次按键都打一条会把日志刷爆;
 * 需要证据时用 --ci-sound-test (它用 INFO 输出判定结果, 不依赖调试构建)。
 */
#include "model_sound.h"
#include "bongo_cat/audio.h"
#include "bongo_cat/model.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

/* 同一条音效 + 同一原因 2 秒内只记一次: 按键重复会把静音日志刷到没有阅读价值。 */
#define SOUND_LOG_INTERVAL_MS 2000

static struct {
    uint64_t at_ms;
    int decision;
    char id[BONGO_CAT_SOUND_ID_CAP];
} sound_log;

static bool sound_log_wanted(const char *id, int decision) {
    uint64_t now = SDL_GetTicks();
    bool same = !strcmp(sound_log.id, id) && sound_log.decision == decision;
    if (same && now - sound_log.at_ms < SOUND_LOG_INTERVAL_MS) return false;
    sound_log.at_ms = now;
    sound_log.decision = decision;
    snprintf(sound_log.id, sizeof(sound_log.id), "%s", id);
    return true;
}

bool bongo_cat_app_sound_id(const BongoCatApp *app, const char *path,
    char *id, size_t capacity) {
    if (!id || !capacity) return false;
    id[0] = '\0';
    if (!app || !path || !path[0]) return false;
    const BongoCatModelEntry *model = app->loaded_model[0]
        ? bongo_cat_models_find(&app->models, app->loaded_model) : NULL;
    const char *model_id = model ? model->id : app->loaded_model;
    const char *directory = NULL;
    if (model) directory = model->adapter_directory[0]
        ? model->adapter_directory : model->directory;
    return bongo_cat_sound_id(id, capacity, model_id, directory, path);
}

bool bongo_cat_sound_status(const BongoCatApp *app, const char *path,
    BongoCatSoundStatus *status) {
    BongoCatSoundStatus computed = {0};
    computed.volume = bongo_cat_settings_sound_volume(
        app ? &app->settings : NULL);
    computed.gain = (float)computed.volume / 100.0f;
    if (app) {
        bongo_cat_app_sound_id(app, path, computed.id, sizeof(computed.id));
        computed.decision = bongo_cat_settings_sound_decision(&app->settings,
            computed.id);
    } else {
        computed.decision = BONGO_CAT_SOUND_DECISION_ALLOWED;
    }
    if (status) *status = computed;
    return path && path[0];
}

bool bongo_cat_app_play_sound(BongoCatApp *app, const char *path, bool overlap) {
    if (!app || !app->audio || !path || !path[0]) return false;
    BongoCatSoundStatus status;
    bongo_cat_sound_status(app, path, &status);
    if (status.decision != BONGO_CAT_SOUND_DECISION_ALLOWED) {
        if (sound_log_wanted(status.id, status.decision))
            SDL_LogDebug(SDL_LOG_CATEGORY_AUDIO,
                "[sound] skip sound id=%s reason=%s", status.id,
                status.decision == BONGO_CAT_SOUND_DECISION_MUTED
                    ? "muted" : "disabled");
        return false;
    }
    if (sound_log_wanted(status.id, status.decision))
        SDL_LogDebug(SDL_LOG_CATEGORY_AUDIO,
            "[sound] would play sound id=%s volume=%d gain=%.3f",
            status.id, status.volume, (double)status.gain);
    bongo_cat_audio_set_volume(app->audio, status.gain);
    BongoCatError error = {0};
    if (bongo_cat_audio_play_sound(app->audio, path, overlap, &error) !=
        BONGO_CAT_OK) {
        if (error.message[0])
            SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "%s", error.message);
        return false;
    }
    return true;
}
