#ifndef BONGO_CAT_PREFERENCES_TEXT_EDIT_H
#define BONGO_CAT_PREFERENCES_TEXT_EDIT_H

#include <stdbool.h>
#include <stddef.h>

typedef enum BongoCatTextEditMove {
    BONGO_CAT_TEXT_EDIT_LEFT,
    BONGO_CAT_TEXT_EDIT_RIGHT,
    BONGO_CAT_TEXT_EDIT_HOME,
    BONGO_CAT_TEXT_EDIT_END
} BongoCatTextEditMove;

void bongo_cat_text_edit_begin(const char *text, size_t *cursor,
    bool *select_all);
bool bongo_cat_text_edit_insert(char *text, size_t capacity, size_t *cursor,
    bool *select_all, const char *inserted);
bool bongo_cat_text_edit_erase(char *text, size_t *cursor,
    bool *select_all, bool forward);
bool bongo_cat_text_edit_move(const char *text, size_t *cursor,
    bool *select_all, BongoCatTextEditMove move);
void bongo_cat_text_edit_select_all(const char *text, size_t *cursor,
    bool *select_all);
void bongo_cat_text_edit_clear(char *text, size_t *cursor, bool *select_all);
void bongo_cat_text_edit_trim(char *text);
size_t bongo_cat_text_edit_nearest(const char *text, float target,
    float (*measure)(const void *userdata, const char *text, size_t length),
    const void *userdata);

#endif
