#ifndef BONGO_CAT_MODEL_SOUND_H
#define BONGO_CAT_MODEL_SOUND_H

#include "bongo_cat/app.h"
#include "bongo_cat/sound.h"

/*
 * 运行时音效策略: 把"这条音效此刻该不该播、以多大增益播"收在一处, 所有真实播放路径
 * (行为快捷键 / 右键菜单 / 随机音效 / 菜单预览提交) 都必须经过它。
 *
 * 为什么不在 bongo_cat_audio_play* 里做: 音频层不知道当前模型、也不知道设置, 而且它
 * 被离线测试直接调用; 策略属于运行时而不是媒体层。
 */

typedef struct BongoCatSoundStatus {
    char id[BONGO_CAT_SOUND_ID_CAP];
    int volume;     /* 0-100, 已钳制 */
    float gain;     /* volume / 100 */
    int decision;   /* BONGO_CAT_SOUND_DECISION_* */
} BongoCatSoundStatus;

/* 计算某个音效文件的稳定 id 与当前判定。app 可以为 NULL (退化成"允许满音量")。
   status 可以为 NULL (只取 id 时用 bongo_cat_app_sound_id 更直接)。 */
bool bongo_cat_sound_status(const BongoCatApp *app, const char *path,
    BongoCatSoundStatus *status);

/* 只算 id (UI 每行要用)。找不到当前模型时仍然返回"模型id:文件名"或文件名。 */
bool bongo_cat_app_sound_id(const BongoCatApp *app, const char *path,
    char *id, size_t capacity);

/*
 * 播放的唯一入口: 判定 -> (允许时) 下发增益 -> 真正起播。
 * 返回 true = 已经交给播放器; false = 被静音/逐项禁用挡住, 或播放器报错。
 * 被挡住时记一行 DEBUG (同一条音效同一原因 2 秒内只记一次, 不刷屏)。
 */
bool bongo_cat_app_play_sound(BongoCatApp *app, const char *path, bool overlap);

#endif
