#ifndef BONGO_CAT_MODEL_BEHAVIOR_CACHE_H
#define BONGO_CAT_MODEL_BEHAVIOR_CACHE_H

#include "bongo_cat/app.h"

bool bongo_cat_model_behavior_cache_matches(const BongoCatApp *app,
    const BongoCatModelEntry *entry);
void bongo_cat_model_behavior_cache_store(BongoCatApp *app,
    const BongoCatModelEntry *entry);

#endif
