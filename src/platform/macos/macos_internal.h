#ifndef BONGO_CAT_MACOS_INTERNAL_H
#define BONGO_CAT_MACOS_INTERNAL_H

#include "bongo_cat/platform.h"

/*
 * macOS 平台层的内部接口(实现分别在 macos_input.m 与 macos.m 里)。
 *
 * 输入采集用 CGEventTap, 而它需要"输入监控"授权: supported 表示系统支持这套 API,
 * monitoring_authorized 查询当前是否已经授权, monitoring_request 会弹出授权提示。上层靠
 * 这三个函数决定是提示用户去授权, 还是走降级路径(只统计窗口内的输入)。
 */
bool bongo_cat_macos_input_start(BongoCatPlatform *platform, BongoCatError *error);
void bongo_cat_macos_input_stop(BongoCatPlatform *platform);
bool bongo_cat_macos_input_supported(void);
bool bongo_cat_macos_input_monitoring_authorized(void);
bool bongo_cat_macos_input_monitoring_request(void);

#endif
