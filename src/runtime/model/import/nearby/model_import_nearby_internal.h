/*
 * nearby 模块内部接口 (不对外暴露): 缓存标记的路径与版本常量, 以及签名与缓存钩子。
 * 契约: 调用方必须给出同一套 target、source 与 signature 才可能命中缓存; signature 是来源的轻量
 * 指纹, identity 是内容的 sha256; 改标记字段时必须一起提升 _SCHEMA。
 */
#ifndef BONGO_CAT_MODEL_IMPORT_NEARBY_INTERNAL_H
#define BONGO_CAT_MODEL_IMPORT_NEARBY_INTERNAL_H

#include "model_import.h"

#define BONGO_CAT_NEARBY_CACHE_MARKER ".bongo-cat-cache.json"
#define BONGO_CAT_NEARBY_CACHE_SCHEMA 7
#define BONGO_CAT_ADAPTER_CACHE_DIRECTORY "model-adapters"

bool bongo_cat_nearby_signature(const BongoCatImportCandidate *candidate,
    char output[65], BongoCatError *error);
bool bongo_cat_nearby_cached_inspection(const char *target,
    const char *source, const char *signature, char identity[65],
    bool *placeholder);
void bongo_cat_nearby_remember_inspection(const char *target,
    const char *source, const char *signature, const char *identity,
    bool placeholder, BongoCatModelMode mode);
bool bongo_cat_nearby_refresh_cache(
    const BongoCatImportCandidate *candidate, const char *cache_root,
    const char *id, const char *source, const char *signature,
    char identity[65], bool placeholder,
    char adapter[BONGO_CAT_PATH_CAP], bool *created, BongoCatError *error);

#endif
