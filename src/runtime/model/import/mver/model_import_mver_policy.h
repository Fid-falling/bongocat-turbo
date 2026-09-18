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
