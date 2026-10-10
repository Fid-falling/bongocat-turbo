#include "bongo_cat/config.h"

#include <string.h>

/*
 * 枚举 <-> 磁盘字符串的唯一映射表。
 *
 * 这些字符串会原样写进 settings.json 并可能被用户或外部工具阅读, 所以只能
 * 新增、不能改已有拼写: 改了会让老配置里的值解析失败。越界的枚举值一律回落到
 * 列表首项, 这几个函数都保证返回非空字符串。
 */
const char *bongo_cat_theme_name(BongoCatTheme value) {
    const char *names[] = {"auto", "light", "dark"};
    return (unsigned)value <= BONGO_CAT_THEME_DARK ? names[value] : names[0];
}

const char *bongo_cat_language_name(BongoCatLanguage value) {
    const char *names[] = {"en-US", "zh-CN", "zh-Hant", "fr-FR", "de-DE",
        "ja-JP", "ko-KR", "pt-BR", "ru-RU", "es-ES"};
    return (unsigned)value < BONGO_CAT_LANG_COUNT ? names[value] : names[0];
}

/* "zh-TW" 是历史写法, 不在 bongo_cat_language_name 的输出里, 但老配置可能
   存着它, 所以这里单独接受一次并映射到繁体中文。 */
bool bongo_cat_language_parse(const char *name, BongoCatLanguage *value) {
    if (!name || !value) return false;
    if (!strcmp(name, "zh-TW")) {
        *value = BONGO_CAT_LANG_ZH_HANT;
        return true;
    }
    for (int i = 0; i < BONGO_CAT_LANG_COUNT; ++i) {
        if (strcmp(name, bongo_cat_language_name((BongoCatLanguage)i)))
            continue;
        *value = (BongoCatLanguage)i;
        return true;
    }
    return false;
}

const char *bongo_cat_renderer_name(BongoCatRenderer value) {
    const char *names[] = {"auto", "opengl", "vulkan"};
    return (unsigned)value < BONGO_CAT_RENDERER_COUNT ? names[value] : names[0];
}

bool bongo_cat_renderer_parse(const char *name, BongoCatRenderer *value) {
    if (!name || !value) return false;
    for (int i = 0; i < BONGO_CAT_RENDERER_COUNT; ++i) {
        if (strcmp(name, bongo_cat_renderer_name((BongoCatRenderer)i)))
            continue;
        *value = (BongoCatRenderer)i;
        return true;
    }
    return false;
}

/*
 * 系统 locale 的匹配规则: 大小写不敏感, 并把 "zh_CN.UTF-8" / "zh-Hans-CN"
 * 这类串按 '-' 和 '_' 切成片段逐个比较, 因此能同时认出 Hant/Hans 和地区码。
 */
static bool locale_part_equal(const char *text, size_t length,
    const char *expected) {
    if (length != strlen(expected)) return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char left = (unsigned char)text[i];
        unsigned char right = (unsigned char)expected[i];
        if (left >= 'A' && left <= 'Z') left += 'a' - 'A';
        if (right >= 'A' && right <= 'Z') right += 'a' - 'A';
        if (left != right) return false;
    }
    return true;
}

static bool locale_has_part(const char *text, const char *expected) {
    if (!text) return false;
    while (*text) {
        while (*text == '-' || *text == '_') text++;
        const char *end = text;
        while (*end && *end != '-' && *end != '_') end++;
        if (locale_part_equal(text, (size_t)(end - text), expected))
            return true;
        text = end;
    }
    return false;
}

static bool locale_language_is(const char *language, const char *expected) {
    if (!language) return false;
    const char *end = language;
    while (*end && *end != '-' && *end != '_') end++;
    return locale_part_equal(language, (size_t)(end - language), expected);
}

/* 简体/繁体的判定顺序不能反: 显式写了 Hans 就一定不是繁体, 只有在没有
   Hans/Hant 标记时才靠地区码(TW/HK/MO)推断。返回 false 表示系统语言不受
   支持, 由调用方决定回落到哪种语言。 */
bool bongo_cat_language_from_locale(const char *language,
    const char *country, BongoCatLanguage *value) {
    if (!language || !value) return false;
    if (locale_language_is(language, "zh")) {
        bool hant = locale_has_part(language, "Hant") ||
            locale_has_part(country, "Hant");
        bool hans = locale_has_part(language, "Hans") ||
            locale_has_part(country, "Hans");
        bool traditional_region = locale_has_part(language, "TW") ||
            locale_has_part(language, "HK") ||
            locale_has_part(language, "MO") ||
            locale_has_part(country, "TW") ||
            locale_has_part(country, "HK") ||
            locale_has_part(country, "MO");
        *value = hant || (!hans && traditional_region) ?
            BONGO_CAT_LANG_ZH_HANT : BONGO_CAT_LANG_ZH_CN;
        return true;
    }
    static const struct {
        const char *code;
        BongoCatLanguage value;
    } supported[] = {{"en", BONGO_CAT_LANG_EN_US},
        {"fr", BONGO_CAT_LANG_FR_FR}, {"de", BONGO_CAT_LANG_DE_DE},
        {"ja", BONGO_CAT_LANG_JA_JP}, {"ko", BONGO_CAT_LANG_KO_KR},
        {"pt", BONGO_CAT_LANG_PT_BR}, {"ru", BONGO_CAT_LANG_RU_RU},
        {"es", BONGO_CAT_LANG_ES_ES}};
    for (size_t i = 0; i < sizeof(supported) / sizeof(supported[0]); ++i)
        if (locale_language_is(language, supported[i].code)) {
            *value = supported[i].value;
            return true;
        }
    return false;
}

const char *bongo_cat_mode_name(BongoCatModelMode value) {
    const char *names[] = {"standard", "keyboard", "gamepad"};
    return (unsigned)value <= BONGO_CAT_MODE_GAMEPAD ? names[value] : names[0];
}

/* 抠像背景色: name 是写进配置的字符串, rgb 是渲染用的 0xRRGGBB 值。
   两张表必须一一对应, 不能只调其中一个的顺序。 */
const char *bongo_cat_obs_background_color_name(
    BongoCatObsBackgroundColor value) {
    static const char *names[] = {
        "#00ff00", "#0000ff", "#ff0000", "#ff00ff"};
    return (unsigned)value < BONGO_CAT_OBS_BACKGROUND_COLOR_COUNT ?
        names[value] : names[BONGO_CAT_OBS_BACKGROUND_GREEN];
}

uint32_t bongo_cat_obs_background_color_rgb(
    BongoCatObsBackgroundColor value) {
    static const uint32_t colors[] = {
        0x00ff00, 0x0000ff, 0xff0000, 0xff00ff};
    return (unsigned)value < BONGO_CAT_OBS_BACKGROUND_COLOR_COUNT ?
        colors[value] : colors[BONGO_CAT_OBS_BACKGROUND_GREEN];
}
