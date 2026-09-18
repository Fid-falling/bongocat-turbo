#include "preferences_text_edit.h"

#include <string.h>

static bool continuation(unsigned char value) {
    return (value & 0xc0) == 0x80;
}

static size_t boundary(const char *text, size_t cursor) {
    size_t length = text ? strlen(text) : 0;
    if (cursor > length) cursor = length;
    while (cursor && cursor < length && continuation((unsigned char)text[cursor]))
        cursor--;
    return cursor;
}

static size_t previous(const char *text, size_t cursor) {
    cursor = boundary(text, cursor);
    if (!cursor) return 0;
    do cursor--; while (cursor && continuation((unsigned char)text[cursor]));
    return cursor;
}

static size_t next(const char *text, size_t cursor) {
    size_t length = text ? strlen(text) : 0;
    cursor = boundary(text, cursor);
    if (cursor >= length) return length;
    do cursor++; while (cursor < length &&
        continuation((unsigned char)text[cursor]));
    return cursor;
}

static size_t rune_bytes(const unsigned char *value, size_t remaining) {
    if (!remaining || !value[0]) return 0;
    if (value[0] < 0x80) return 1;
    size_t count = value[0] >= 0xc2 && value[0] <= 0xdf ? 2 :
        value[0] >= 0xe0 && value[0] <= 0xef ? 3 :
        value[0] >= 0xf0 && value[0] <= 0xf4 ? 4 : 0;
    if (!count || count > remaining) return 0;
    for (size_t i = 1; i < count; ++i)
        if (!continuation(value[i])) return 0;
    if ((count == 3 && value[0] == 0xe0 && value[1] < 0xa0) ||
        (count == 3 && value[0] == 0xed && value[1] >= 0xa0) ||
        (count == 4 && value[0] == 0xf0 && value[1] < 0x90) ||
        (count == 4 && value[0] == 0xf4 && value[1] >= 0x90)) return 0;
    return count;
}

void bongo_cat_text_edit_begin(const char *text, size_t *cursor,
    bool *select_all) {
    if (cursor) *cursor = text ? strlen(text) : 0;
    if (select_all) *select_all = false;
}

bool bongo_cat_text_edit_insert(char *text, size_t capacity, size_t *cursor,
    bool *select_all, const char *inserted) {
    if (!text || !capacity || !cursor || !select_all || !inserted) return false;
    bool changed = false;
    if (*select_all) {
        changed = text[0] != '\0';
        text[0] = '\0'; *cursor = 0; *select_all = false;
    }
    size_t length = strlen(text);
    *cursor = boundary(text, *cursor);
    size_t remaining = strlen(inserted);
    for (const unsigned char *source = (const unsigned char *)inserted;
        remaining && *source;) {
        size_t bytes = rune_bytes(source, remaining);
        if (!bytes) { source++; remaining--; continue; }
        if (bytes == 1 && source[0] < 32) {
            source++; remaining--; continue;
        }
        if (length + bytes >= capacity) break;
        memmove(text + *cursor + bytes, text + *cursor,
            length - *cursor + 1);
        memcpy(text + *cursor, source, bytes);
        *cursor += bytes; length += bytes;
        source += bytes; remaining -= bytes; changed = true;
    }
    return changed;
}

bool bongo_cat_text_edit_erase(char *text, size_t *cursor,
    bool *select_all, bool forward) {
    if (!text || !cursor || !select_all) return false;
    size_t length = strlen(text);
    if (*select_all) {
        bool changed = length != 0;
        text[0] = '\0'; *cursor = 0; *select_all = false;
        return changed;
    }
    *cursor = boundary(text, *cursor);
    size_t start = forward ? *cursor : previous(text, *cursor);
    size_t end = forward ? next(text, *cursor) : *cursor;
    if (start == end) return false;
    memmove(text + start, text + end, length - end + 1);
    *cursor = start;
    return true;
}

bool bongo_cat_text_edit_move(const char *text, size_t *cursor,
    bool *select_all, BongoCatTextEditMove move) {
    if (!text || !cursor || !select_all) return false;
    size_t length = strlen(text), old = *cursor;
    bool selected = *select_all;
    *cursor = boundary(text, *cursor);
    if (move == BONGO_CAT_TEXT_EDIT_HOME || (selected &&
        move == BONGO_CAT_TEXT_EDIT_LEFT)) *cursor = 0;
    else if (move == BONGO_CAT_TEXT_EDIT_END || (selected &&
        move == BONGO_CAT_TEXT_EDIT_RIGHT)) *cursor = length;
    else if (move == BONGO_CAT_TEXT_EDIT_LEFT) *cursor = previous(text, *cursor);
    else if (move == BONGO_CAT_TEXT_EDIT_RIGHT) *cursor = next(text, *cursor);
    *select_all = false;
    return selected || old != *cursor;
}

void bongo_cat_text_edit_select_all(const char *text, size_t *cursor,
    bool *select_all) {
    if (cursor) *cursor = text ? strlen(text) : 0;
    if (select_all) *select_all = true;
}

void bongo_cat_text_edit_clear(char *text, size_t *cursor, bool *select_all) {
    if (text) text[0] = '\0';
    if (cursor) *cursor = 0;
    if (select_all) *select_all = false;
}

void bongo_cat_text_edit_trim(char *text) {
    if (!text) return;
    size_t start = 0, end = strlen(text);
    while (start < end && (text[start] == ' ' || text[start] == '\t')) start++;
    while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t'))
        end--;
    if (start) memmove(text, text + start, end - start);
    text[end - start] = '\0';
}

size_t bongo_cat_text_edit_nearest(const char *text, float target,
    float (*measure)(const void *userdata, const char *text, size_t length),
    const void *userdata) {
    if (!text || !measure || target <= 0.0f) return 0;
    size_t length = strlen(text), cursor = 0;
    float previous_width = 0.0f;
    while (cursor < length) {
        size_t after = next(text, cursor);
        float width = measure(userdata, text, after);
        if (target < previous_width + (width - previous_width) * 0.5f)
            return cursor;
        previous_width = width; cursor = after;
    }
    return length;
}
