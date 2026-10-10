#ifndef BONGO_CAT_MACOS_KEYS_H
#define BONGO_CAT_MACOS_KEYS_H

#include <ApplicationServices/ApplicationServices.h>

/*
 * CGKeyCode(物理键码) -> 事件名。
 *
 * 用键码而不是字符: 键码只跟物理位置有关, 不受键盘布局和修饰键影响 —— 同一个键在 ABC /
 * 中文 / Dvorak 布局下键码相同, 字符却会变。名字与 Windows / Linux 两侧共用同一套字符串,
 * 配置文件才能跨平台通用。返回 NULL 表示不认识这个键。
 */
const char *bongo_cat_macos_key_name(CGKeyCode code, char output[16]);

#endif
