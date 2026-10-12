/* 存储目录解析的对外接口 (实现见 storage_paths.c)。
   prepare 必须在日志与配置之前调用; apply_cache 必须在配置加载之后调用 —— 它要读
   settings.cache_root。 */
#ifndef BONGO_CAT_STORAGE_PATHS_H
#define BONGO_CAT_STORAGE_PATHS_H

#include "bongo_cat/app.h"

/*
 * 六棵存储树 (config/data/models/cache/state/logs)。拆成结构体是为了把"解析"与"落到
 * app 上"分开: 平台默认目录、便携根、--storage-root 走的都是同一个 trees_under;
 * 关闭便携模式时也要靠它拿到"数据该并回哪里"。
 */
typedef struct BongoCatStorageTrees {
    char config[BONGO_CAT_PATH_CAP];
    char data[BONGO_CAT_PATH_CAP];
    char models[BONGO_CAT_PATH_CAP];
    char cache[BONGO_CAT_PATH_CAP];
    char state[BONGO_CAT_PATH_CAP];
    char logs[BONGO_CAT_PATH_CAP];
} BongoCatStorageTrees;

bool bongo_cat_storage_paths_prepare(BongoCatApp *app,
    BongoCatError *error);
/* 平台默认的六棵树, 忽略 --storage-root 与便携模式。只有"关闭便携模式时把数据并回
   默认目录"这一条路径需要它 (见 portable_mode.c), 别处不要用它绕过解析顺序。 */
bool bongo_cat_storage_paths_platform_trees(BongoCatStorageTrees *trees);
/* Re-apply the cache directory from the current settings (or CLI override) and
   ensure it is writable. Returns false if the configured directory is unusable. */
bool bongo_cat_storage_paths_apply_cache(BongoCatApp *app,
    BongoCatError *error);

#endif
