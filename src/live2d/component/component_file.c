/*
 * 组件里的文件打开: 桥接要读模型/着色器等资源, 用的是宿主的 `bongo_cat_file_open`
 * (UTF-8 路径 + Windows 宽字符转换)。组件不能链接宿主的实现(那会连带宿主的 CRT 与平台层),
 * 所以这里自己实现一份, 语义与宿主那份**必须完全一致**: 传入 UTF-8, 内部转宽字符,
 * 失败返回 NULL。少这一步, 带中文的用户目录就会读不到文件 —— 这是最容易漏的一处。
 *
 * 只实现桥接真正用到的那一个函数(打开); 删除/替换/追加都由宿主负责, 组件不需要。
 */
#include "bongo_cat/file.h"

#include <stdio.h>

#ifdef _WIN32
#include "windows_utf8.h"
#include <stdlib.h>
#include <wchar.h>

FILE *bongo_cat_file_open(const char *path, const char *mode) {
    if (!path || !path[0] || !mode || !mode[0]) return NULL;
    wchar_t *wide_path = bongo_cat_windows_wide(path);
    if (!wide_path) return NULL;
    wchar_t wide_mode[8] = {0};
    /* 模式串是 ASCII 且极短; 逐个字符转宽字符, 目标缓冲比任何合法模式都大。 */
    for (size_t i = 0; i < sizeof(wide_mode) / sizeof(wide_mode[0]) - 1 && mode[i];
        ++i) wide_mode[i] = (wchar_t)(unsigned char)mode[i];
    FILE *file = _wfopen(wide_path, wide_mode);
    free(wide_path);
    return file;
}
#else
FILE *bongo_cat_file_open(const char *path, const char *mode) {
    if (!path || !path[0] || !mode || !mode[0]) return NULL;
    return fopen(path, mode);
}
#endif
