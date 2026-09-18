#ifndef BONGO_CAT_MODEL_CATALOG_SELECTION_H
#define BONGO_CAT_MODEL_CATALOG_SELECTION_H

#include "runtime.h"

typedef struct BongoCatModelSelection {
    char additional[BONGO_CAT_ADDITIONAL_MODEL_CAP][BONGO_CAT_ID_CAP];
    size_t count;
} BongoCatModelSelection;

BongoCatModelSelection bongo_cat_model_selection_capture(
    const BongoCatApp *app);
void bongo_cat_model_selection_restore(BongoCatApp *app,
    const BongoCatModelSelection *selection);
bool bongo_cat_model_catalog_reconcile(BongoCatApp *app);

#endif
