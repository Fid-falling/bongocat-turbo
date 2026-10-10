#include "bongo_cat/utf8.h"

#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

/*
 * UTF-8 校验, 以及把 GBK 编码的模型文件"救回来"的转换。
 *
 * bongo_cat_utf8_valid 是严格的 RFC 3629 校验: 拒绝过长编码(0xC0/0xC1)、
 * UTF-16 代理区(0xED 0xA0..0xBF)和超出 U+10FFFF 的序列。配置、模型 id、
 * 快捷键都靠它把关, 所以判断只能收紧不能放松 —— 放进来一个非法序列, 后面
 * 所有字符串处理都会跟着错位。
 */
static bool continuation(unsigned char value) {
    return (value & 0xc0u) == 0x80u;
}

/* 0xE0 / 0xED / 0xF0 / 0xF4 这几个首字节必须单独判第二个字节的取值范围:
   它们正好横跨过长编码和代理区, 用统一区间判断会把非法序列放进来。 */
bool bongo_cat_utf8_valid(const char *text) {
    if (!text) return false;
    const unsigned char *value = (const unsigned char *)text;
    while (*value) {
        if (*value <= 0x7f) { value++; continue; }
        if (*value >= 0xc2 && *value <= 0xdf && continuation(value[1])) {
            value += 2; continue;
        }
        if (*value == 0xe0 && value[1] >= 0xa0 && value[1] <= 0xbf &&
            continuation(value[2])) { value += 3; continue; }
        if (((*value >= 0xe1 && *value <= 0xec) ||
             (*value >= 0xee && *value <= 0xef)) &&
            continuation(value[1]) && continuation(value[2])) {
            value += 3; continue;
        }
        if (*value == 0xed && value[1] >= 0x80 && value[1] <= 0x9f &&
            continuation(value[2])) { value += 3; continue; }
        if (*value == 0xf0 && value[1] >= 0x90 && value[1] <= 0xbf &&
            continuation(value[2]) && continuation(value[3])) {
            value += 4; continue;
        }
        if (*value >= 0xf1 && *value <= 0xf3 && continuation(value[1]) &&
            continuation(value[2]) && continuation(value[3])) {
            value += 4; continue;
        }
        if (*value == 0xf4 && value[1] >= 0x80 && value[1] <= 0x8f &&
            continuation(value[2]) && continuation(value[3])) {
            value += 4; continue;
        }
        return false;
    }
    return true;
}

/*
 * Mver 模型自带的 JSON 有可能是 GBK 而不是 UTF-8(在中文 Windows 上被别的
 * 工具重新保存过就会这样)。先按 UTF-8 校验, 不通过再用代码页 936 解码后转
 * 回 UTF-8, 并且逐字符缩短重试: 尾部被截断的半个字符会让整次转换失败, 丢掉
 * 最后一个字符也比整个模型读不出来强。
 * 失败时 output 被写成空串(不是未定义内容), 调用方可以安全地当空值用。
 */
bool bongo_cat_utf8_normalize_mver(const char *text,
    char *output, size_t capacity) {
    if (!text || !output || !capacity) return false;
    size_t length = strlen(text);
    if (bongo_cat_utf8_valid(text)) {
        if (length >= capacity) return false;
        memcpy(output, text, length + 1);
        return true;
    }
#ifdef _WIN32
    if (length >= 128) return false;
    wchar_t wide[128];
/* 936 = 简体中文 GBK 代码页。这是 Windows 专有的兜底, 其它平台没有对应
   机制, 只能判定失败。 */
    int wide_length = MultiByteToWideChar(936, 0, text, (int)length,
        wide, (int)(sizeof(wide) / sizeof(wide[0])));
    if (wide_length <= 0) return false;
    for (int count = wide_length; count > 0; --count) {
        int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            wide, count, output, (int)capacity - 1, NULL, NULL);
        if (bytes > 0) {
            output[bytes] = '\0';
            return bongo_cat_utf8_valid(output);
        }
    }
#else
    (void)length;
#endif
    output[0] = '\0';
    return false;
}
