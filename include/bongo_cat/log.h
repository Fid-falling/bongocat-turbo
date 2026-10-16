#ifndef BONGO_CAT_LOG_H
#define BONGO_CAT_LOG_H

#include <SDL3/SDL_log.h>

enum {
    BONGO_CAT_LOG_LIFECYCLE = SDL_LOG_CATEGORY_CUSTOM,
    BONGO_CAT_LOG_UPDATE,
    BONGO_CAT_LOG_INPUT
};

static inline bool bongo_cat_log_enabled(int category, SDL_LogPriority priority) {
#ifdef NDEBUG
    return priority >= SDL_LOG_PRIORITY_WARN ||
        (priority == SDL_LOG_PRIORITY_INFO &&
            (category == BONGO_CAT_LOG_LIFECYCLE ||
                category == BONGO_CAT_LOG_UPDATE ||
                category == BONGO_CAT_LOG_INPUT));
#else
    /* Debug 构建放行 DEBUG 及以上日志, 便于观察控制流细节; 不区分分类。 */
    (void)category;
    return priority >= SDL_LOG_PRIORITY_DEBUG;
#endif
}

#endif