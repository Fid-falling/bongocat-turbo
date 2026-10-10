/* 系统语言探测, 只用于"第一次运行 / 配置损坏"时的默认语言: 用户显式选过的语言永远
   优先 (见 config_store.c 的加载逻辑), 否则换系统语言会把用户的选择冲掉。
   SDL_GetPreferredLocales 已按用户偏好排序, 取第一个能识别的就停。 */
#include "runtime.h"

#include <SDL3/SDL.h>

bool bongo_cat_system_language(BongoCatLanguage *language) {
    if (!language) return false;
    int count = 0;
    SDL_Locale **locales = SDL_GetPreferredLocales(&count);
    bool matched = false;
/* 逐个匹配, 全部认不出来就返回 false, 由调用方保留原来的语言值。 */
    for (int i = 0; locales && i < count; ++i) {
        const SDL_Locale *locale = locales[i];
        if (locale && bongo_cat_language_from_locale(locale->language,
                locale->country, language)) {
            matched = true;
            break;
        }
    }
    if (locales) SDL_free(locales);
    return matched;
}
