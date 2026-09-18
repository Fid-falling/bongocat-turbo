#include "runtime.h"

#include <SDL3/SDL.h>

bool bongo_cat_system_language(BongoCatLanguage *language) {
    if (!language) return false;
    int count = 0;
    SDL_Locale **locales = SDL_GetPreferredLocales(&count);
    bool matched = false;
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
