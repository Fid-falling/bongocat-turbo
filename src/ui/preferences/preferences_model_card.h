#ifndef BONGO_CAT_PREFERENCES_MODEL_CARD_H
#define BONGO_CAT_PREFERENCES_MODEL_CARD_H

#include "bongo_cat/model.h"
#include "bongo_cat/preferences.h"

struct nk_context;

bool bongo_cat_preferences_model_import_card(BongoCatPreferences *value,
    struct nk_context *context);
bool bongo_cat_preferences_model_section(BongoCatPreferences *value,
    struct nk_context *context);
void bongo_cat_preferences_model_card(BongoCatPreferences *value,
    struct nk_context *context, const BongoCatModelEntry *entry,
    bool storage_busy);
void bongo_cat_preferences_model_select(BongoCatPreferences *value,
    const BongoCatModelEntry *entry);

#endif
