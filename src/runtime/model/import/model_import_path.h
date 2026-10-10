/* 源格式模块共用的小路径工具：取父目录、取相对路径、后缀比较。
 * Windows 上比较折叠大小写且把 '/' 与 '\' 视为等价 —— 这是本文件最容易踩错的约定。 */
#ifndef BONGO_CAT_MODEL_IMPORT_PATH_H
#define BONGO_CAT_MODEL_IMPORT_PATH_H

#include <stdbool.h>
#include <stddef.h>

/* Small path operations shared by source-format modules. */
bool bongo_cat_import_parent_path(const char *path, char *parent,
    size_t capacity);
bool bongo_cat_import_relative_path(const char *root, const char *path,
    char *relative, size_t capacity);
bool bongo_cat_import_has_suffix(const char *value, const char *suffix);
bool bongo_cat_import_has_suffix_ci(const char *value, const char *suffix);

#endif
