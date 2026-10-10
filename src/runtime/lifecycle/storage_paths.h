/* 存储目录解析的对外接口 (实现见 storage_paths.c)。
   prepare 必须在日志与配置之前调用; apply_cache 必须在配置加载之后调用 —— 它要读
   settings.cache_root。 */
#ifndef BONGO_CAT_STORAGE_PATHS_H
#define BONGO_CAT_STORAGE_PATHS_H

#include "bongo_cat/app.h"

bool bongo_cat_storage_paths_prepare(BongoCatApp *app,
    BongoCatError *error);
/* Re-apply the cache directory from the current settings (or CLI override) and
   ensure it is writable. Returns false if the configured directory is unusable. */
bool bongo_cat_storage_paths_apply_cache(BongoCatApp *app,
    BongoCatError *error);

#endif
