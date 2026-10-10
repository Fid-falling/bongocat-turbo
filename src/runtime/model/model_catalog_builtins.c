/* 把程序自带的模型（assets/models 下的 standard、keyboard、gamepad）落地到可写的
 * models_root，并写入 .bongo-cat-builtin 标记文件。时序上由扫描线程在目录扫描之前调用，
 * 且必须持有导入锁；标记文件是 core/model_catalog.c 判定 preset 的唯一依据，丢了它内置
 * 模型会退化成用户模型，从而允许被删除或改名。 */
#include "model_storage.h"
#include "bongo_cat/file.h"
#include "bongo_cat/path.h"

#include <stdio.h>
#include <string.h>

/* 幂等：标记已存在就直接返回成功（目录可能来自旧版本或用户手工放置），不校验目录内容；
 * 任何一步 IO 失败都会写入 error 并返回 false。 */
static bool mark_builtin(const char *directory, BongoCatError *error) {
    char path[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(path, sizeof(path), directory,
        BONGO_CAT_MODEL_BUILTIN_MARKER)) return false;
    if (bongo_cat_path_is_file(path)) return true;
    FILE *file = bongo_cat_file_open(path, "wb");
    bool ok = file && fputs("BongoCat built-in model\n", file) >= 0;
    if (file && fclose(file) != 0) ok = false;
    if (!ok) bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
        "Cannot mark built-in model: %s", directory);
    return ok;
}

/* 启动扫描专用，需由调用方持有导入锁（会复制整棵目录）。目标目录已存在时跳过复制但仍补
 * 写标记 —— 升级后的旧内置模型就是靠这一步被认领；内置资源缺失立即中止并返回 IO。 */
BongoCatResult bongo_cat_model_install_builtins(const char *asset_root,
    const char *models_root, BongoCatError *error) {
    static const char *const names[] = {"standard", "keyboard", "gamepad"};
    char source_root[BONGO_CAT_PATH_CAP];
    if (!asset_root || !models_root ||
        !bongo_cat_path_join(source_root, sizeof(source_root), asset_root,
            "models") || !bongo_cat_path_create_directory(models_root))
        return BONGO_CAT_ERROR_ARGUMENT;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        char source[BONGO_CAT_PATH_CAP], target[BONGO_CAT_PATH_CAP];
        if (!bongo_cat_path_join(source, sizeof(source), source_root, names[i]) ||
            !bongo_cat_path_join(target, sizeof(target), models_root, names[i]) ||
            !bongo_cat_path_is_dir(source)) {
            bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
                "Built-in model assets are missing: %s", names[i]);
            return BONGO_CAT_ERROR_IO;
        }
        if (!bongo_cat_path_is_dir(target)) {
            if (bongo_cat_path_is_file(target) ||
                bongo_cat_model_copy_directory(source, target, error) != BONGO_CAT_OK)
                return error && error->code ? error->code : BONGO_CAT_ERROR_IO;
        }
        if (!mark_builtin(target, error))
            return error && error->code ? error->code : BONGO_CAT_ERROR_IO;
    }
    return BONGO_CAT_OK;
}
