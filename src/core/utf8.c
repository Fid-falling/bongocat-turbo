#include "bongo_cat/utf8.h"

#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static bool continuation(unsigned char value) {
    return (value & 0xc0u) == 0x80u;
}

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
