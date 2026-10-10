#include "bongo_cat/update.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>

/*
 * 版本号解析与比较(只处理语义化版本 x.y.z[-预发布][+构建元数据])。
 *
 * 为什么不能直接比字符串: "1.10.0" 在字符串序里小于 "1.9.0", 会让用户永远
 * 收不到更新。所以三段核心号各自解析成 64 位整数, 缺段按 0 补齐("1.2" 与
 * "1.2.0" 视为相等)。
 *
 * 契约: 解析失败一律 valid=false; 比较函数遇到非法版本返回 0(相等), 所以调用
 * 方必须先自行确认版本串合法, 不能把 0 当成"确定相等"。
 */
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

/* 语义化版本的数字段不允许前导零("01" 就是非法版本), 并且要挡溢出: 版本号
   来自网络返回的 JSON, 不能假设它讲道理。 */
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

/* 预发布段和构建元数据段按 '.' 分片校验: 每片非空、只含字母数字与 '-',
   纯数字片同样禁止前导零。这里只保证格式合法, 不做语义解释。 */
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

/* 允许开头带一个 'v'(GitHub 的 tag 习惯)。解析完必须正好走到字符串末尾,
   任何多余字符都让整个版本作废 —— 宁可判非法, 也不要"尽量解析"出一个错误
   的版本号。 */
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

/* 语义化版本的标识符比较规则: 纯数字按数值比, 数字段永远小于字母段, 其余按
   字典序。顺序搞反会让 beta 被认为比正式版新。 */
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

/* 没有预发布段 = 正式版, 比任何带预发布段的都大(1.0.0 > 1.0.0-rc1)。两边都有
   时逐片比较, 先耗尽的一边更小(1.0.0-alpha < 1.0.0-alpha.1)。 */
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

/* 返回值: 负数表示 left 更旧, 正数表示 left 更新, 0 表示相等或无法比较。 */
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
