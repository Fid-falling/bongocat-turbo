#ifndef BONGO_CAT_MODEL_IMPORT_TAURI_H
#define BONGO_CAT_MODEL_IMPORT_TAURI_H

#include "../model_import.h"

/* Source-format adapter. Conversion output must be a canonical Mver package. */
int bongo_cat_import_tauri_discover_exact(const char *source,
    BongoCatImportDiscovery *discovery, BongoCatError *error);
int bongo_cat_import_tauri_discover_recursive(const char *source,
    BongoCatImportDiscovery *discovery, BongoCatError *error);
bool bongo_cat_import_tauri_convert_to_mver(
    const BongoCatImportCandidate *candidate, const char *target,
    BongoCatImportCandidate *installed, BongoCatError *error);

#endif
