/* Mver (BongoCat 原版) 模型包的发现与适配入口: standard/keyboard/gamepad 三模式共用同一份候选结构。
 * 四个 discover 沿用发现器约定 (大于 0 命中, 0 表示"不是这种格式, 继续试别的适配器", 负值为错误), 就地填充
 * 调用方清零过的 discovery; _exact 版只看 source 自身, 非 exact 版会向上找包根 (补丁版则受限地扫子树)。 */
#ifndef BONGO_CAT_MODEL_IMPORT_MVER_H
#define BONGO_CAT_MODEL_IMPORT_MVER_H

#include "../model_import.h"

/* Canonical package discovery and runtime-adapter generation. */
bool bongo_cat_import_mver_config_path(const char *root,
    char *path, size_t capacity);
int bongo_cat_import_mver_discover(const char *source,
    BongoCatImportDiscovery *discovery, BongoCatError *error);
int bongo_cat_import_mver_discover_exact(const char *source,
    BongoCatImportDiscovery *discovery, BongoCatError *error);
int bongo_cat_import_mver_patch_discover(const char *source,
    BongoCatImportDiscovery *discovery, BongoCatError *error);
int bongo_cat_import_mver_patch_discover_exact(const char *source,
    BongoCatImportDiscovery *discovery, BongoCatError *error);
bool bongo_cat_import_mver_assets(const BongoCatImportCandidate *candidate,
    const char *target, BongoCatError *error);

#endif
