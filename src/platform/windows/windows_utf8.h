#ifndef BONGO_CAT_WINDOWS_UTF8_H
#define BONGO_CAT_WINDOWS_UTF8_H

#ifdef _WIN32
#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

/*
 * UTF-8 <-> UTF-16 转换, Windows 平台层所有字符串进出的唯一通道。
 *
 * 契约: bongo_cat_windows_wide 返回新分配的宽字符串, 由调用方 free, 失败返回 NULL;
 * bongo_cat_windows_utf8 写进调用方的缓冲区, 空间不够就返回 false(绝不截断)。
 * 两者都带 MB_ERR_INVALID_CHARS / WC_ERR_INVALID_CHARS: 非法编码直接失败, 而不是被
 * 悄悄替换成 '?', 否则路径会指向另一个文件。
 */
wchar_t *bongo_cat_windows_wide(const char *text);
/*
 * 纯文本 (命令行参数、URL 之类) 的 UTF-8 -> UTF-16: 不做 '/' -> '\\' 归一, 也不加 \\?\ 前缀。
 * 参数里的正斜杠是语法的一部分, 用路径版本去转会得到 "/c" -> "\c" 这种完全不同的东西。
 * 同样返回新分配的宽字符串, 由调用方 free, 失败返回 NULL。
 */
wchar_t *bongo_cat_windows_wide_text(const char *text);
bool bongo_cat_windows_utf8(const wchar_t *text, char *output, size_t capacity);
#endif

#endif
