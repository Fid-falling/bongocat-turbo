/*
 * Mver 内部策略: 内置模型指纹比对、图片补丁的底层包指纹, 以及补丁是否已有完整基准。
 * 三个函数都只读 candidate, 由 model_import_digest、identity、installed 与 nearby 调用。
 * digest 缓存可以为 NULL, 此时退化成每次都算真实 sha256。
 */
#ifndef BONGO_CAT_MODEL_IMPORT_MVER_POLICY_H
#define BONGO_CAT_MODEL_IMPORT_MVER_POLICY_H

#include "model_import_mver.h"

bool bongo_cat_import_mver_stock_model(
    const BongoCatImportCandidate *candidate,
    BongoCatImportDigestCache *cache);
bool bongo_cat_import_patch_base_inspect(
    const BongoCatImportCandidate *candidate, char output[65],
    bool *placeholder, BongoCatError *error);
bool bongo_cat_import_patch_has_full_base(
    const BongoCatImportDiscovery *discovery, size_t candidate_index);

#endif
