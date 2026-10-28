#ifndef BONGO_CAT_AUDIO_H
#define BONGO_CAT_AUDIO_H

#include "bongo_cat/common.h"

/*
 * 音频播放接口。
 *
 * 契约: 全部函数都在 app 线程调用 (内部的 voice 池没有锁); play 系列是**非阻塞**的 ——
 * miniaudio 异步解码, 所以"文件不存在/解码失败"不会立刻体现在返回值上, 而是在之后的
 * bongo_cat_audio_update() 里以日志形式出现。
 * update() 必须被周期性调用: 空闲回收与延迟错误上报都由它驱动。set_enabled(false) 会连带
 * 重置 (卸载) 音频引擎。
 */
typedef struct BongoCatAudio BongoCatAudio;

BongoCatAudio *bongo_cat_audio_create(BongoCatError *error);
void bongo_cat_audio_destroy(BongoCatAudio *audio);
/*
 * 自检: 用一个**临时引擎**试初始化一次音频设备, 成功即销毁。
 * 与正在播放的引擎完全隔离 (不碰 audio 的状态), 所以可以在音效正在播的时候调用。
 * 失败时 error 里是 miniaudio 的返回码与描述 —— 那正是音频故障唯一有价值的线索。
 */
bool bongo_cat_audio_probe(BongoCatError *error);
BongoCatResult bongo_cat_audio_play(BongoCatAudio *audio, const char *path, BongoCatError *error);
/* With overlap disabled, only the same clip restarts; other clips continue. */
BongoCatResult bongo_cat_audio_play_sound(BongoCatAudio *audio, const char *path,
    bool overlap, BongoCatError *error);
void bongo_cat_audio_update(BongoCatAudio *audio);
void bongo_cat_audio_reset(BongoCatAudio *audio);
void bongo_cat_audio_stop(BongoCatAudio *audio);
bool bongo_cat_audio_any_playing(const BongoCatAudio *audio);
bool bongo_cat_audio_is_playing(const BongoCatAudio *audio, const char *path);
void bongo_cat_audio_stop_path(BongoCatAudio *audio, const char *path);
void bongo_cat_audio_set_enabled(BongoCatAudio *audio, bool enabled);
/*
 * 全局播放增益 (0.0 - 1.0)。作用在**本程序自己的 miniaudio 引擎**上, 不碰系统音量,
 * 也不影响其它进程; 已经起播的 voice 也会立刻跟着变 (引擎级音量), 面板上拖动滑杆
 * 时不会出现"旧声音还按老音量放完"。
 * 引擎是懒初始化的: 未初始化时只记下值, 首次起播时补下发 (见 audio_playback.c)。
 */
void bongo_cat_audio_set_volume(BongoCatAudio *audio, float volume);

#endif
