#ifndef BONGO_CAT_RUNTIME_STATE_H
#define BONGO_CAT_RUNTIME_STATE_H

#include "bongo_cat/app.h"
#include <stddef.h>

void bongo_cat_runtime_timestamp(char *target, size_t capacity);
void bongo_cat_runtime_state_previous(BongoCatApp *app, char *state,
    size_t capacity);
void bongo_cat_runtime_state_clean(BongoCatApp *app, const char *timestamp);

#endif
