/* 模型存储的底层文件操作（实现见 model_storage.c）：递归删除、清理导入残留、递归复制、安
 * 装内置模型。除只读调用外都需要在 bongo_cat_import_storage_lock() 保护下使用。 */
#ifndef BONGO_CAT_MODEL_STORAGE_H
#define BONGO_CAT_MODEL_STORAGE_H

#include "bongo_cat/common.h"

bool bongo_cat_model_remove_tree(const char *path, BongoCatError *error);
bool bongo_cat_model_cleanup_imports(const char *root, BongoCatError *error);
BongoCatResult bongo_cat_model_copy_directory(const char *source,
    const char *target, BongoCatError *error);
BongoCatResult bongo_cat_model_install_builtins(const char *asset_root,
    const char *models_root, BongoCatError *error);

#endif
