#ifndef BONGO_CAT_SOUND_H
#define BONGO_CAT_SOUND_H

#include "bongo_cat/config.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 音效管理与"这一条音效现在要不要播"的纯逻辑 (不依赖 SDL / miniaudio)。
 *
 * 唯一标识 (settings.sound.disabled 里保存的字符串) 的规则:
 *   "<模型id>:<音效文件相对模型适配器目录的路径>"
 * 例如 "A-清宵:resources/sounds/0.flac"。
 *
 * 为什么是相对路径而不是下标: 模型重新导出后 bindings 数组的顺序可能变, 下标会让
 * "用户关掉的那个音效"悄悄指向另一个文件; 文件名/相对路径在同一个模型里是稳定的。
 * 前面加模型 id 是因为多个模型可能都有 resources/sounds/0.flac, 不加就会互相误伤。
 * 不在模型目录下的路径退回文件名 —— 仍然比数字下标稳定。
 */
#define BONGO_CAT_SOUND_DECISION_ALLOWED 0
#define BONGO_CAT_SOUND_DECISION_MUTED 1
#define BONGO_CAT_SOUND_DECISION_DISABLED 2

/* 生成跨会话稳定的音效 id。model_directory 可以为空 (那就只用文件名); 生成失败
   (参数非法/放不下) 返回 false 并把 output 置空。路径分隔符统一写成 '/',
   这样同一份配置跨平台读写不会产生两个不同的 id。 */
bool bongo_cat_sound_id(char *output, size_t capacity,
    const char *model_id, const char *model_directory, const char *path);

/* 该 id 是否在"逐个关掉"的集合里。空 id / 空设置一律返回 false (默认全开)。 */
bool bongo_cat_settings_sound_disabled(const BongoCatSettings *settings,
    const char *id);

/* 写入/清除一个 id; disabled=true 时追加(已存在则保持), false 时删除。
   id 非法或表满返回 false。调用后调用方负责让配置落盘 (settings_hash 会看到变化)。 */
bool bongo_cat_settings_set_sound_disabled(BongoCatSettings *settings,
    const char *id, bool disabled);

/* 此刻该不该播: 全部静音优先于逐项禁用, 两者都不命中就是允许 (音量 0 仍然算允许 ——
   静音是一个开关, 音量是增益, 混在一起会让"静音后恢复音量"变得不可预期)。 */
int bongo_cat_settings_sound_decision(const BongoCatSettings *settings,
    const char *id);

/* 当前生效的总音量, 已按 0-100 钳制 (读配置时校验过一次, 这里再兜底)。 */
int bongo_cat_settings_sound_volume(const BongoCatSettings *settings);

#ifdef __cplusplus
}
#endif

#endif
