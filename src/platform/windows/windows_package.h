#ifndef BONGO_CAT_WINDOWS_PACKAGE_H
#define BONGO_CAT_WINDOWS_PACKAGE_H

#ifdef _WIN32
#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

/*
 * MSIX / 应用包相关的路径判定。
 *
 * 打包版(MSIX)运行在容器里, 可写目录被重定向到包自己的 LocalCache 下, 所以"配置该
 * 放哪"与便携版 / 安装版不同。相关 API 都是运行时从 kernel32 动态取的(见 .c), 因为
 * 老系统上根本没有这些符号 —— 静态导入会让程序直接起不来。
 */
bool bongo_cat_windows_is_packaged(void);
bool bongo_cat_windows_package_storage_root(char *output, size_t capacity);
bool bongo_cat_windows_package_storage_root_for(const wchar_t *profile,
    const wchar_t *family, char *output, size_t capacity);
#endif

#endif
