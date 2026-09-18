#include "test.h"
#include "bongo_cat/config.h"

static void check_locale(const char *language, const char *country,
    BongoCatLanguage expected) {
    BongoCatLanguage actual = BONGO_CAT_LANG_COUNT;
    CHECK(bongo_cat_language_from_locale(language, country, &actual));
    CHECK(actual == expected);
}

void test_language(void) {
    check_locale("en", "GB", BONGO_CAT_LANG_EN_US);
    check_locale("fr-CA", NULL, BONGO_CAT_LANG_FR_FR);
    check_locale("de", "AT", BONGO_CAT_LANG_DE_DE);
    check_locale("ja", "JP", BONGO_CAT_LANG_JA_JP);
    check_locale("ko", "KR", BONGO_CAT_LANG_KO_KR);
    check_locale("pt", "PT", BONGO_CAT_LANG_PT_BR);
    check_locale("ru", "RU", BONGO_CAT_LANG_RU_RU);
    check_locale("es", "MX", BONGO_CAT_LANG_ES_ES);
    check_locale("zh", "CN", BONGO_CAT_LANG_ZH_CN);
    check_locale("zh", "SG", BONGO_CAT_LANG_ZH_CN);
    check_locale("zh", "TW", BONGO_CAT_LANG_ZH_HANT);
    check_locale("zh", "HK", BONGO_CAT_LANG_ZH_HANT);
    check_locale("ZH-hAnT", "CN", BONGO_CAT_LANG_ZH_HANT);
    check_locale("zh-Hans", "TW", BONGO_CAT_LANG_ZH_CN);
    check_locale("zh", "Hant", BONGO_CAT_LANG_ZH_HANT);
    BongoCatLanguage unchanged = BONGO_CAT_LANG_DE_DE;
    CHECK(!bongo_cat_language_from_locale("it", "IT", &unchanged));
    CHECK(unchanged == BONGO_CAT_LANG_DE_DE);
    CHECK(!bongo_cat_language_from_locale(NULL, NULL, &unchanged));
}
