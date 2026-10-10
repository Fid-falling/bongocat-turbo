#ifndef BONGO_CAT_WINDOWS_KEYS_H
#define BONGO_CAT_WINDOWS_KEYS_H

#ifdef _WIN32
#include <stdbool.h>
#include <windows.h>

/*
 * 原始键码 -> 事件名 / 索引的映射。
 *
 * 索引空间固定 1024: 它由 MakeCode 与 VKey 组合而成, 用来给每台设备记录"这个键现在
 * 是否按着"。这个上限必须不小于 windows_keys.c 里所有可能算出的下标。
 */
#define BONGO_CAT_WINDOWS_RAW_KEY_COUNT 1024

const char *bongo_cat_windows_key_name(const RAWKEYBOARD *key, char output[16]);
unsigned bongo_cat_windows_key_index(const RAWKEYBOARD *key);
/* 查询鼠标右键此刻是否按下: 命中测试据此临时取消穿透, 让用户还能拖动窗口。 */
bool bongo_cat_windows_right_button_down(void);
#endif
#endif
