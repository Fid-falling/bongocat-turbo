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
