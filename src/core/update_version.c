#include "bongo_cat/update.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>

typedef struct ParsedVersion {
    uint64_t core[3];
    const char *prerelease;
    size_t prerelease_length;
    bool valid;
} ParsedVersion;

static bool identifier_character(char value) {
    unsigned char byte = (unsigned char)value;
    return isalnum(byte) || value == '-';
}

static bool parse_number(const char **cursor, uint64_t *number) {
    const char *start = *cursor;
    if (!isdigit((unsigned char)*start)) return false;
    if (*start == '0' && isdigit((unsigned char)start[1])) return false;
    uint64_t value = 0;
    while (isdigit((unsigned char)**cursor)) {
        unsigned digit = (unsigned)(**cursor - '0');
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
        (*cursor)++;
    }
    *number = value;
    return true;
}

static bool validate_identifiers(const char *value, size_t length) {
    if (!length) return false;
    size_t identifier_start = 0;
    for (size_t i = 0; i <= length; ++i) {
        if (i < length && value[i] != '.') {
            if (!identifier_character(value[i])) return false;
            continue;
        }
        if (i == identifier_start) return false;
        bool numeric = true;
        for (size_t j = identifier_start; j < i; ++j)
            if (!isdigit((unsigned char)value[j])) numeric = false;
        if (numeric && i - identifier_start > 1 &&
            value[identifier_start] == '0') return false;
        identifier_start = i + 1;
    }
    return true;
}

static ParsedVersion parse_version(const char *text) {
    ParsedVersion parsed = {0};
    if (!text || !text[0]) return parsed;
    const char *cursor = text;
    if (*cursor == 'v' || *cursor == 'V') cursor++;
    for (size_t i = 0; i < 3; ++i) {
        if (!parse_number(&cursor, &parsed.core[i])) return parsed;
        if (i < 2) {
            if (*cursor != '.') return parsed;
            cursor++;
        }
    }
    if (*cursor == '-') {
        const char *start = ++cursor;
        while (*cursor && *cursor != '+') cursor++;
        parsed.prerelease = start;
        parsed.prerelease_length = (size_t)(cursor - start);
        if (!validate_identifiers(start, parsed.prerelease_length))
            return (ParsedVersion){0};
    }
    if (*cursor == '+') {
        const char *start = ++cursor;
        while (*cursor) cursor++;
        if (!validate_identifiers(start, (size_t)(cursor - start)))
            return (ParsedVersion){0};
    }
    parsed.valid = *cursor == '\0';
    return parsed;
}

static int compare_identifier(const char *left, size_t left_length,
    const char *right, size_t right_length) {
    bool left_numeric = true, right_numeric = true;
    for (size_t i = 0; i < left_length; ++i)
        if (!isdigit((unsigned char)left[i])) left_numeric = false;
    for (size_t i = 0; i < right_length; ++i)
        if (!isdigit((unsigned char)right[i])) right_numeric = false;
    if (left_numeric != right_numeric) return left_numeric ? -1 : 1;
    if (left_numeric && left_length != right_length)
        return left_length < right_length ? -1 : 1;
    size_t common = left_length < right_length ? left_length : right_length;
    int compared = memcmp(left, right, common);
    if (compared) return compared < 0 ? -1 : 1;
    return left_length == right_length ? 0 : left_length < right_length ? -1 : 1;
}

static int compare_prerelease(const ParsedVersion *left,
    const ParsedVersion *right) {
    if (!left->prerelease_length || !right->prerelease_length) {
        if (left->prerelease_length == right->prerelease_length) return 0;
        return left->prerelease_length ? -1 : 1;
    }
    size_t left_at = 0, right_at = 0;
    while (left_at < left->prerelease_length &&
        right_at < right->prerelease_length) {
        size_t left_end = left_at, right_end = right_at;
        while (left_end < left->prerelease_length &&
            left->prerelease[left_end] != '.') left_end++;
        while (right_end < right->prerelease_length &&
            right->prerelease[right_end] != '.') right_end++;
        int compared = compare_identifier(left->prerelease + left_at,
            left_end - left_at, right->prerelease + right_at,
            right_end - right_at);
        if (compared) return compared;
        left_at = left_end + 1;
        right_at = right_end + 1;
    }
    bool left_done = left_at > left->prerelease_length;
    bool right_done = right_at > right->prerelease_length;
    return left_done == right_done ? 0 : left_done ? -1 : 1;
}

int bongo_cat_update_compare_versions(const char *left, const char *right) {
    ParsedVersion parsed_left = parse_version(left);
    ParsedVersion parsed_right = parse_version(right);
    if (!parsed_left.valid || !parsed_right.valid) return 0;
    for (size_t i = 0; i < 3; ++i) {
        if (parsed_left.core[i] == parsed_right.core[i]) continue;
        return parsed_left.core[i] < parsed_right.core[i] ? -1 : 1;
    }
    return compare_prerelease(&parsed_left, &parsed_right);
}

bool bongo_cat_update_version_valid(const char *version) {
    return parse_version(version).valid;
}
