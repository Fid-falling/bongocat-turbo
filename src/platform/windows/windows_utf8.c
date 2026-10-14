#include "windows_utf8.h"

#ifdef _WIN32
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/*
 * 顺带把 '/' 统一成 '\\': 调用方(尤其是 core 的路径拼接)可能产生正斜杠, 而很多
 * Windows API 只认反斜杠。
 * 超过 240 字符的长路径要加 \\?\ 前缀才能突破 MAX_PATH: 盘符路径加 \\?\, UNC 路径
 * 必须写成 \\?\UNC\server\share(所以先去掉了原来的两个反斜杠, 再加 8 字符前缀)。
 * 阈值取 240 而不是 260, 是给调用方后续拼接留余量。
 */
wchar_t *bongo_cat_windows_wide(const char *text) {
    int length = text ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        text, -1, NULL, 0) : 0;
    wchar_t *wide = length > 0 ? malloc((size_t)length * sizeof(*wide)) : NULL;
    if (wide && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        text, -1, wide, length)) {
        free(wide); return NULL;
    }
    if (!wide) return NULL;
    for (wchar_t *value = wide; *value; ++value)
        if (*value == L'/') *value = L'\\';
    bool drive = length > 3 && wide[1] == L':' && wide[2] == L'\\';
    bool unc = length > 3 && wide[0] == L'\\' && wide[1] == L'\\' &&
        wide[2] != L'?';
    if (length <= 240 || (!drive && !unc)) return wide;
    size_t prefix = unc ? 8 : 4, skip = unc ? 2 : 0;
    wchar_t *extended = malloc(((size_t)length + prefix - skip) * sizeof(*extended));
    if (!extended) { free(wide); return NULL; }
    memcpy(extended, unc ? L"\\\\?\\UNC\\" : L"\\\\?\\", prefix * sizeof(*extended));
    memcpy(extended + prefix, wide + skip, ((size_t)length - skip) * sizeof(*extended));
    free(wide); return extended;
}

/*
 * 纯文本的 UTF-8 -> UTF-16: 与 bongo_cat_windows_wide 的唯一区别是**不做路径归一**
 * (不把 '/' 换成 '\\', 也不加 \\?\ 前缀)。
 *
 * 为什么必须有两个: bongo_cat_windows_wide 是给文件路径用的, 而命令行参数不是路径 ——
 * 参数里的正斜杠是有意义的语法 (cmd 的 /c、URL、--switch=/value 这类写法), 被换成反斜杠
 * 之后程序收到的就是另一个东西了 (实测: 重启参数里的 "/c" 变成 "\c", cmd 什么也不做)。
 * 同理也不能给它加 \\?\ 前缀, 那会变成参数字符串的一部分。
 */
wchar_t *bongo_cat_windows_wide_text(const char *text) {
    int length = text ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        text, -1, NULL, 0) : 0;
    wchar_t *wide = length > 0 ? malloc((size_t)length * sizeof(*wide)) : NULL;
    if (!wide) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide,
            length)) {
        free(wide);
        return NULL;
    }
    return wide;
}

/* 先问需要多少字节再转换, 因此容量不够时直接失败, 不会写出半截字符串。 */
bool bongo_cat_windows_utf8(const wchar_t *text, char *output, size_t capacity) {
    if (!text || !output || !capacity) return false;
    int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        text, -1, NULL, 0, NULL, NULL);
    return required > 0 && (size_t)required <= capacity &&
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            text, -1, output, (int)capacity, NULL, NULL) > 0;
}
#endif
