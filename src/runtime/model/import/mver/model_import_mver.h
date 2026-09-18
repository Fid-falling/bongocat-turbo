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
