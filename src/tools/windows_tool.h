#ifndef BONGO_CAT_WINDOWS_TOOL_H
#define BONGO_CAT_WINDOWS_TOOL_H

#include <wchar.h>

/* 构建期 / 验证期工具专用的辅助函数(刻意不链接产品运行时)。成功返回 1。 */
int bongo_cat_tool_ensure_directory(const wchar_t *path);

#endif
