/*
 * 播放一个音频文件: 选槽位、装载、起播。
 *
 * 播放是非阻塞的 (MA_SOUND_FLAG_ASYNC), 所以"文件不存在/解码失败"不会在这里立刻变成返回值,
 * 而是稍后由 bongo_cat_audio_collect 查到结果码时以日志形式暴露 —— 调用方不能假设返回成功
 * 就一定出得了声。
 */
#include "audio_internal.h"

#include <SDL3/SDL.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

/* 三个标志各有理由: STREAM 让解码内存与音频时长无关 (按键音可能很长); ASYNC 保证不阻塞 UI
 * 线程 —— 桌宠掉帧比晚十毫秒出声严重得多; NO_SPATIALIZATION 因为这是纯 2D 场景, 不需要
 * 3D 声像计算。 */
static ma_result load_voice(BongoCatAudio *audio, AudioVoice *voice, const char *path) {
    ma_uint32 flags = MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_ASYNC |
        MA_SOUND_FLAG_NO_SPATIALIZATION;
#ifdef _WIN32
    /* Windows 上必须自己转宽字符: ma_sound_init_from_file 走 ANSI 代码页, 含中文/日文的
     * 路径会直接失败 (踩过的坑)。 */
    wchar_t wide[BONGO_CAT_PATH_CAP];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
        wide, BONGO_CAT_PATH_CAP)) return MA_INVALID_ARGS;
    return ma_sound_init_from_file_w(&audio->engine, wide, flags, NULL, NULL, &voice->sound);
#else
    return ma_sound_init_from_file(&audio->engine, path, flags, NULL, NULL, &voice->sound);
#endif
}

BongoCatResult bongo_cat_audio_play_sound(BongoCatAudio *audio, const char *path,
    bool overlap, BongoCatError *error) {
    if (!audio || !path || !path[0] || strlen(path) >= BONGO_CAT_PATH_CAP) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_ARGUMENT, "Invalid audio path or player");
        return BONGO_CAT_ERROR_ARGUMENT;
    }
    if (!audio->enabled) return BONGO_CAT_OK;
    if (!audio->initialized) {
        ma_result result = ma_engine_init(NULL, &audio->engine);
        if (result != MA_SUCCESS) {
            bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
                "Audio initialization failed: %d", result);
            return BONGO_CAT_ERROR_PLATFORM;
        }
        audio->initialized = true;
        /* 引擎重建后必须立刻补上当前增益: 用户可能是在引擎空闲卸载之后才拖的音量
           滑杆 (那时 set_volume 只更新了字段), 漏掉这一步会以满音量播第一条。 */
        ma_engine_set_volume(&audio->engine, audio->volume);
    }
    /* 三段式选槽: 同一个文件且允许打断则复用 (顺带 seek 回起点重播) -> 否则取一个空闲的 ->
     * 都不空闲就抢 order 最小的那一个 (sequence 单调递增, 所以 order 最小即最久未用)。 */
    AudioVoice *selected = NULL, *idle = NULL, *oldest = &audio->voices[0];
    for (size_t i = 0; i < AUDIO_VOICES; ++i) {
        AudioVoice *voice = &audio->voices[i];
        bool playing = voice->ready && ma_sound_is_playing(&voice->sound) &&
            !ma_sound_at_end(&voice->sound);
        if (voice->ready && strcmp(voice->path, path) == 0 && (!overlap || !playing)) {
            selected = voice;
            break;
        }
        if (!playing && (!idle || !voice->ready)) idle = voice;
        if (voice->order < oldest->order) oldest = voice;
    }
    if (!selected) {
        selected = idle ? idle : oldest;
        bongo_cat_audio_voice_release(selected);
        ma_result result = load_voice(audio, selected, path);
        if (result != MA_SUCCESS) {
            bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
                "Cannot load audio (%d): %s", result, path);
            return BONGO_CAT_ERROR_IO;
        }
        selected->ready = true;
        memcpy(selected->path, path, strlen(path) + 1);
    } else {
        ma_sound_stop(&selected->sound);
        ma_result result = ma_sound_seek_to_pcm_frame(&selected->sound, 0);
        if (result != MA_SUCCESS) {
            bongo_cat_audio_voice_release(selected);
            bongo_cat_error_set(error, BONGO_CAT_ERROR_IO, "Cannot rewind audio: %s", path);
            return BONGO_CAT_ERROR_IO;
        }
    }
    /* 两个时间戳都是回收策略的输入: used 决定单个 voice 何时可回收, last_play 决定整体何时
     * 算空闲。漏更新任何一个, 引擎都会被过早卸载或永远不卸载。 */
    selected->used = audio->last_play = SDL_GetTicks();
    selected->order = ++audio->sequence;
    if (ma_sound_start(&selected->sound) != MA_SUCCESS) {
        bongo_cat_audio_voice_release(selected);
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM, "Cannot start audio: %s", path);
        return BONGO_CAT_ERROR_PLATFORM;
    }
    return BONGO_CAT_OK;
}

BongoCatResult bongo_cat_audio_play(BongoCatAudio *audio, const char *path, BongoCatError *error) {
    return bongo_cat_audio_play_sound(audio, path, false, error);
}

