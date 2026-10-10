#include "bongo_cat/i18n.h"
#include "bongo_cat/file.h"
#include "bongo_cat/path.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <yyjson.h>

/*
 * 界面文案的加载与查表。
 *
 * 一个语言文件就是一份嵌套 JSON, key 以 native. / pages. / components. /
 * composables. 开头。en-US 永远加载并充当 fallback, active 指向当前语言;
 * 当前语言就是 en-US 时 active 直接复用 fallback 那份文档, 因此 destroy 和
 * reload 里处处都要判断 active != fallback —— 少了这个判断就会把同一份文档
 * free 两次。
 *
 * 契约: 源码里出现的每个 key 都必须存在于每一种语言的 locale 文件里,
 * cmake/CheckLocalization.cmake 会在构建时校验(见该脚本)。
 */
struct BongoCatI18n {
    char root[BONGO_CAT_PATH_CAP];
    BongoCatLanguage language;
    yyjson_doc *fallback;
    yyjson_doc *active;
};

/* 文件名就是 BongoCatLanguage 对应的语言串(见 config_names.c): 枚举值和
   磁盘上的文件名是同一个契约的两端, 改一边就要改另一边。 */
static yyjson_doc *load_locale(const char *root, BongoCatLanguage language,
    BongoCatError *error) {
    char name[32], path[BONGO_CAT_PATH_CAP];
    snprintf(name, sizeof(name), "%s.json", bongo_cat_language_name(language));
    if (!bongo_cat_path_join(path, sizeof(path), root, name)) return NULL;
    yyjson_read_err json_error = {0};
    FILE *file = bongo_cat_file_open(path, "rb");
    yyjson_doc *doc = file ? yyjson_read_fp(file, 0, NULL, &json_error) : NULL;
    if (file) fclose(file);
    if (!doc) bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
        "Cannot load locale %s: %s", path,
        json_error.msg ? json_error.msg : "cannot open file");
    return doc;
}

BongoCatI18n *bongo_cat_i18n_create(const char *root, BongoCatLanguage language,
    BongoCatError *error) {
    if (!root) return NULL;
    BongoCatI18n *value = calloc(1, sizeof(*value));
    if (!value) return NULL;
    snprintf(value->root, sizeof(value->root), "%s", root);
    value->fallback = load_locale(root, BONGO_CAT_LANG_EN_US, error);
    if (!value->fallback) {
        free(value);
        return NULL;
    }
    if (bongo_cat_i18n_reload(value, language, error) != BONGO_CAT_OK) {
        bongo_cat_i18n_destroy(value);
        return NULL;
    }
    return value;
}

void bongo_cat_i18n_destroy(BongoCatI18n *value) {
    if (!value) return;
    if (value->active && value->active != value->fallback) yyjson_doc_free(value->active);
    yyjson_doc_free(value->fallback);
    free(value);
}

BongoCatResult bongo_cat_i18n_reload(BongoCatI18n *value, BongoCatLanguage language,
    BongoCatError *error) {
    if (!value) return BONGO_CAT_ERROR_ARGUMENT;
    yyjson_doc *next = language == BONGO_CAT_LANG_EN_US
        ? value->fallback : load_locale(value->root, language, error);
    if (!next) return BONGO_CAT_ERROR_FORMAT;
    if (value->active && value->active != value->fallback) yyjson_doc_free(value->active);
    value->active = next;
    value->language = language;
    return BONGO_CAT_OK;
}

/* 按 '.' 逐级下钻。单级名字超过 63 字节一律当找不到 —— key 都是编译期
   字面量, 超长只可能是拼错了; 中途遇到非对象也直接返回 NULL。 */
static yyjson_val *find_value(yyjson_doc *doc, const char *key) {
    yyjson_val *value = doc ? yyjson_doc_get_root(doc) : NULL;
    const char *cursor = key;
    while (value && cursor && *cursor) {
        const char *dot = strchr(cursor, '.');
        size_t length = dot ? (size_t)(dot - cursor) : strlen(cursor);
        char part[64];
        if (!length || length >= sizeof(part)) return NULL;
        memcpy(part, cursor, length);
        part[length] = '\0';
        value = yyjson_is_obj(value) ? yyjson_obj_get(value, part) : NULL;
        cursor = dot ? dot + 1 : NULL;
    }
    return value;
}

/*
 * 回退链: 当前语言 -> en-US -> 调用方给的 fallback 字符串。任何一级拿到的不
 * 是字符串(缺键或类型不对)就继续往下退, 所以只要调用方的 fallback 非空, 这个
 * 函数就不会返回 NULL, 调用方可以直接用返回值。
 */
const char *bongo_cat_i18n_get(const BongoCatI18n *value, const char *key,
    const char *fallback) {
    if (!value || !key) return fallback;
    yyjson_val *found = find_value(value->active, key);
    if (!yyjson_is_str(found)) found = find_value(value->fallback, key);
    return yyjson_is_str(found) ? yyjson_get_str(found) : fallback;
}

/*
 * 下面是给字体用的部分: 把语言文件里出现过的所有码点收集起来, 合并成若干
 * [first, last] 区间交给渲染层, 这样不必为每种语言都加载完整字形表。
 * 非法序列解码成 0, 会被 add_point 丢掉, 所以坏数据不会污染区间表。
 */
static uint32_t decode_utf8(const unsigned char **cursor) {
    const unsigned char *s = *cursor;
    uint32_t value;
    if (*s < 0x80) { *cursor = s + 1; return *s; }
    if ((*s & 0xe0) == 0xc0) {
        value = (uint32_t)(*s & 0x1f); s++;
        if ((*s & 0xc0) != 0x80) return 0;
        value = (value << 6) | (*s++ & 0x3f);
    } else if ((*s & 0xf0) == 0xe0) {
        value = (uint32_t)(*s & 0x0f); s++;
        for (int i = 0; i < 2; ++i) {
            if ((*s & 0xc0) != 0x80) return 0;
            value = (value << 6) | (*s++ & 0x3f);
        }
    } else if ((*s & 0xf8) == 0xf0) {
        value = (uint32_t)(*s & 0x07); s++;
        for (int i = 0; i < 3; ++i) {
            if ((*s & 0xc0) != 0x80) return 0;
            value = (value << 6) | (*s++ & 0x3f);
        }
    } else { *cursor = s + 1; return 0; }
    *cursor = s;
    return value;
}

/* 丢掉 ASCII 可打印区(0x20..0x7E 由 build_ranges 固定加入)、越界码点和重复
   项; 4096 个码点是这张表的硬预算, 超出的一律不要。 */
static void add_point(uint32_t *points, size_t *count, uint32_t point) {
    if (point <= 0x7e || point > 0x10ffff || *count >= 4096) return;
    for (size_t i = 0; i < *count; ++i) if (points[i] == point) return;
    points[(*count)++] = point;
}

static void collect_value(yyjson_val *value, uint32_t *points, size_t *count) {
    if (yyjson_is_str(value)) {
        const unsigned char *cursor = (const unsigned char *)yyjson_get_str(value);
        while (*cursor) add_point(points, count, decode_utf8(&cursor));
    } else if (yyjson_is_arr(value)) {
        size_t index, maximum; yyjson_val *item;
        yyjson_arr_foreach(value, index, maximum, item) collect_value(item, points, count);
    } else if (yyjson_is_obj(value)) {
        size_t index, maximum; yyjson_val *key, *item;
        yyjson_obj_foreach(value, index, maximum, key, item) collect_value(item, points, count);
    }
}

static int compare_point(const void *left, const void *right) {
    uint32_t a = *(const uint32_t *)left, b = *(const uint32_t *)right;
    return a < b ? -1 : a > b;
}

/*
 * 把码点排序后合并成 [first, last] 区间数组, 末尾补一个 0 作哨兵。
 * builtins 里的各语言母语名必须写死在这儿: 它们要显示在语言选择菜单上, 而
 * 那一刻可能还没有任何语言文件被加载成功。
 * capacity 不够时只写完整区间就收尾, 返回值包含哨兵那一位。
 */
static size_t build_ranges(uint32_t *points, size_t count,
    uint32_t *ranges, size_t capacity) {
    size_t written = 2;
    const unsigned char *builtins =
        (const unsigned char *)"English Fran\xC3\xA7" "ais Deutsch "
        "Portugu\xC3\xAAs Espa\xC3\xB1" "ol"
        "\xE7\xAE\x80\xE4\xBD\x93\xE4\xB8\xAD\xE6\x96\x87"
        "\xE7\xB9\x81\xE9\xAB\x94\xE4\xB8\xAD\xE6\x96\x87"
        "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"
        "\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4"
        "\xD0\xA0\xD1\x83\xD1\x81\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9";
    while (*builtins) add_point(points, &count, decode_utf8(&builtins));
    qsort(points, count, sizeof(points[0]), compare_point);
    ranges[0] = 0x20; ranges[1] = 0x7e;
    size_t i = 0;
    while (i < count && points[i] <= 0x7e) i++;
    while (i < count && written + 2 < capacity) {
        uint32_t first = points[i], last = first;
        while (++i < count && points[i] <= last + 1)
            if (points[i] > last) last = points[i];
        ranges[written++] = first;
        ranges[written++] = last;
    }
    ranges[written] = 0;
    return written + 1;
}

size_t bongo_cat_i18n_glyph_ranges(const BongoCatI18n *value, uint32_t *ranges,
    size_t capacity) {
    if (!value || !ranges || capacity < 3) return 0;
    uint32_t points[4096]; size_t count = 0;
    collect_value(yyjson_doc_get_root(value->active), points, &count);
    return build_ranges(points, count, ranges, capacity);
}

/* 把所有语言的文件都读一遍取并集, 用于"字体要覆盖全部语言"的场景。
   某个语言包损坏时直接跳过(错误写进局部变量丢弃), 不因此让整次加载失败。 */
size_t bongo_cat_i18n_all_glyph_ranges(const BongoCatI18n *value, uint32_t *ranges,
    size_t capacity) {
    if (!value || !ranges || capacity < 3) return 0;
    uint32_t points[4096]; size_t count = 0;
    collect_value(yyjson_doc_get_root(value->fallback), points, &count);
    for (int language = 0; language < BONGO_CAT_LANG_COUNT; ++language) {
        if (language == BONGO_CAT_LANG_EN_US) continue;
        BongoCatError ignored = {0};
        yyjson_doc *doc = load_locale(value->root, (BongoCatLanguage)language,
            &ignored);
        if (doc) { collect_value(yyjson_doc_get_root(doc), points, &count);
            yyjson_doc_free(doc); }
    }
    return build_ranges(points, count, ranges, capacity);
}
