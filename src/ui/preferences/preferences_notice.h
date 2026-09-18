#ifndef BONGO_CAT_PREFERENCES_NOTICE_H
#define BONGO_CAT_PREFERENCES_NOTICE_H

#include "bongo_cat/app.h"
#include "bongo_cat/preferences.h"
#include "nuklear_config.h"

void bongo_cat_preferences_notice_show(BongoCatApp *app,
    const char *message, bool error);
void bongo_cat_preferences_notice_draw(BongoCatPreferences *preferences,
    struct nk_context *context, float width, float height);

#endif
