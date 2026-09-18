#ifndef BONGO_CAT_MODEL_IMPORT_MVER_COPY_H
#define BONGO_CAT_MODEL_IMPORT_MVER_COPY_H

#include "model_import_mver.h"

bool bongo_cat_import_mver_copy_package(
    const BongoCatImportCandidate *candidate, const char *target,
    BongoCatImportCandidate *installed, BongoCatError *error);

#endif
