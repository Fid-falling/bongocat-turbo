#include "model_import_digest_internal.h"

#include <string.h>

static bool separator(char value) {
    return value == '/' || value == '\\';
}

static bool character_equal(char left, char right) {
    if (separator(left) && separator(right)) return true;
#ifdef _WIN32
    if (left >= 'A' && left <= 'Z') left = (char)(left - 'A' + 'a');
    if (right >= 'A' && right <= 'Z') right = (char)(right - 'A' + 'a');
#endif
    return left == right;
}

size_t bongo_cat_import_digest_path_length(const char *path) {
    size_t length = path ? strlen(path) : 0;
    while (length > 1 && separator(path[length - 1])) length--;
    return length;
}

bool bongo_cat_import_digest_path_contains(const char *root,
    const char *path) {
    size_t root_length = bongo_cat_import_digest_path_length(root);
    size_t item_length = bongo_cat_import_digest_path_length(path);
    if (!root_length || item_length < root_length) return false;
    for (size_t i = 0; i < root_length; ++i)
        if (!character_equal(root[i], path[i])) return false;
    return item_length == root_length || separator(path[root_length]);
}

uint64_t bongo_cat_import_digest_hash_text(const char *value) {
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char *cursor = (const unsigned char *)value;
        cursor && *cursor; ++cursor) {
        unsigned char byte = *cursor == '\\' ? '/' : *cursor;
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

uint64_t bongo_cat_import_digest_mix(uint64_t value) {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ull;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}
