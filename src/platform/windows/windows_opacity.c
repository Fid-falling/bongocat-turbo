#include "bongo_cat/platform.h"

#ifdef _WIN32
float bongo_cat_platform_get_opacity(const BongoCatPlatform *platform) {
    return platform ? platform->window_opacity : 1.0f;
}
#endif
