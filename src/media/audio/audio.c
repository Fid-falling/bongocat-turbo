/*
 * 音频播放器的生命周期与空闲回收。
 *
 * 全部状态由 app 线程独占 (无锁)。collect 是唯一的回收点, 最多每秒跑一次, 负责三件事:
 * 发现异步解码的失败、回收播完的 voice、整体空闲时卸载引擎 —— 于是闲下来的桌宠既不会占着
 * 音频设备, 也不会留着解码线程。
 */
#include "audio_internal.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

/* 释放后 memset 置零, 这就是"槽位空了"的唯一判据 (ready == false): 池里没有别的空闲链表,
 * 所以以后新增字段也会被这一把清零自动复位。 */
/* App-thread owned. Streaming bounds decoded memory independently of clip
   duration; the pool bounds decoder/file handles during rapid key presses. */
void bongo_cat_audio_voice_release(AudioVoice *voice) {
    if (!voice->ready) return;
    ma_sound_stop(&voice->sound);
    /* uninit waits for pending decoding jobs before releasing their data. */
    ma_sound_uninit(&voice->sound);
    memset(voice, 0, sizeof(*voice));
}

BongoCatAudio *bongo_cat_audio_create(BongoCatError *error) {
    BongoCatAudio *audio = calloc(1, sizeof(*audio));
    if (!audio) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY, "Cannot allocate audio player");
        return NULL;
    }
    audio->enabled = true;
    audio->volume = 1.0f;
    return audio;
}

/*
 * 全局增益: 记在 audio 上并 (若引擎已初始化) 立刻下发。用引擎级音量而不是逐个 voice,
 * 是为了让"正在播的声音"也实时跟随滑杆, 而且不需要在播放路径上反复设值。
 * 只影响本程序的 miniaudio 引擎 —— 系统音量与其它进程完全不受影响。
 */
void bongo_cat_audio_set_volume(BongoCatAudio *audio, float volume) {
    if (!audio) return;
    if (!(volume >= 0.0f)) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    audio->volume = volume;
    if (audio->initialized) ma_engine_set_volume(&audio->engine, volume);
}

void bongo_cat_audio_stop(BongoCatAudio *audio) {
    if (!audio) return;
    for (size_t i = 0; i < AUDIO_VOICES; ++i) bongo_cat_audio_voice_release(&audio->voices[i]);
}

/* 卸载引擎并让限频计时归零, 这样重新开始播放后第一次回收不会被旧的节流窗口挡掉。
 * 只有已初始化时才 uninit —— miniaudio 的 uninit 对未初始化的引擎不是无操作。 */
void bongo_cat_audio_reset(BongoCatAudio *audio) {
    if (!audio) return;
    bongo_cat_audio_stop(audio);
    if (audio->initialized) ma_engine_uninit(&audio->engine);
    audio->initialized = false;
    audio->next_collect = 0;
}

/* 唯一的回收点, 由 bongo_cat_audio_update 驱动。用外部传入的 now 而不是自己取时钟, 是为了
 * 让离线测试里的回收时机可确定。解码失败只有在这里才查得到 —— miniaudio 是异步初始化,
 * 当时的返回值只说明"任务已排队"。 */
void bongo_cat_audio_collect(BongoCatAudio *audio, uint64_t now) {
    if (!audio || !audio->initialized || now < audio->next_collect) return;
    audio->next_collect = now + 1000;
    bool active = false;
    for (size_t i = 0; i < AUDIO_VOICES; ++i) {
        AudioVoice *voice = &audio->voices[i];
        if (!voice->ready) continue;
        ma_result result = ma_resource_manager_data_source_result(
            voice->sound.pResourceManagerDataSource);
        if (result != MA_SUCCESS && result != MA_BUSY) {
            SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "Cannot decode audio (%d): %s", result, voice->path);
            bongo_cat_audio_voice_release(voice);
        } else if (ma_sound_at_end(&voice->sound) || !ma_sound_is_playing(&voice->sound)) {
            if (now - voice->used >= AUDIO_IDLE_MS) bongo_cat_audio_voice_release(voice);
        } else active = true;
    }
    /* Idle players retain neither device callbacks nor decoder workers. */
    if (!active && now - audio->last_play >= AUDIO_IDLE_MS) bongo_cat_audio_reset(audio);
}

void bongo_cat_audio_update(BongoCatAudio *audio) {
    bongo_cat_audio_collect(audio, SDL_GetTicks());
}

void bongo_cat_audio_set_enabled(BongoCatAudio *audio, bool enabled) {
    if (!audio) return;
    audio->enabled = enabled;
    if (!enabled) bongo_cat_audio_reset(audio);
}

void bongo_cat_audio_destroy(BongoCatAudio *audio) {
    if (!audio) return;
    bongo_cat_audio_reset(audio);
    free(audio);
}

bool bongo_cat_audio_is_playing(const BongoCatAudio *audio, const char *path) {
    if (!audio || !path || !path[0]) return false;
    for (size_t i = 0; i < AUDIO_VOICES; ++i) {
        const AudioVoice *voice = &audio->voices[i];
        if (!voice->ready || strcmp(voice->path, path)) continue;
        ma_result result = ma_resource_manager_data_source_result(voice->sound.pResourceManagerDataSource);
        if ((result == MA_SUCCESS || result == MA_BUSY) &&
            ma_sound_is_playing(&voice->sound) && !ma_sound_at_end(&voice->sound)) return true;
    }
    return false;
}

void bongo_cat_audio_stop_path(BongoCatAudio *audio, const char *path) {
    if (!audio || !path || !path[0]) return;
    for (size_t i = 0; i < AUDIO_VOICES; ++i)
        if (audio->voices[i].ready && !strcmp(audio->voices[i].path, path))
            bongo_cat_audio_voice_release(&audio->voices[i]);
}

bool bongo_cat_audio_any_playing(const BongoCatAudio *audio) {
    if (!audio) return false;
    for (size_t i = 0; i < AUDIO_VOICES; ++i) {
        const AudioVoice *voice = &audio->voices[i];
        if (!voice->ready) continue;
        ma_result result = ma_resource_manager_data_source_result(voice->sound.pResourceManagerDataSource);
        if ((result == MA_SUCCESS || result == MA_BUSY) &&
            ma_sound_is_playing(&voice->sound) && !ma_sound_at_end(&voice->sound)) return true;
    }
    return false;
}
