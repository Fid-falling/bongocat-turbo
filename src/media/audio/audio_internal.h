/*
 * 音频模块的内部定义: 只给 src/media/audio/*.c 用, 公共 API 在 include/bongo_cat/audio.h。
 * 全部状态由 app 线程独占 —— 没有锁, 这是不变量: 任何回调里都不要去碰 voice 池。
 */
#ifndef BONGO_CAT_AUDIO_INTERNAL_H
#define BONGO_CAT_AUDIO_INTERNAL_H

#include "bongo_cat/audio.h"
#include <miniaudio.h>

/* 池容量是给"快速连按"封顶的: 它同时限制了解码器与打开的文件句柄数量, 满了就按最旧者抢占,
 * 所以内存占用不会随按键速度增长。 */
#define AUDIO_VOICES 50
/* 静置这么久就把 voice 还回去; 整体都空闲时连引擎一起卸载, 免得桌宠常驻占着音频设备。 */
#define AUDIO_IDLE_MS 5000

typedef struct AudioVoice {
    ma_sound sound;
    char path[BONGO_CAT_PATH_CAP];
    uint64_t used;
    uint64_t order;
    bool ready;
} AudioVoice;

/* sequence / last_play / next_collect 是回收策略的输入: order 用来定"谁最旧",
 * last_play 判断整体是否进入空闲, next_collect 给 collect 限频。 */
struct BongoCatAudio {
    ma_engine engine;
    AudioVoice voices[AUDIO_VOICES];
    uint64_t sequence;
    uint64_t last_play;
    uint64_t next_collect;
    bool initialized;
    bool enabled;
};

void bongo_cat_audio_voice_release(AudioVoice *voice);

/* Explicit time keeps idle reclamation deterministic in offline tests. */
void bongo_cat_audio_collect(BongoCatAudio *audio, uint64_t now_ms);

#endif
