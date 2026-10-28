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

/*
 * 崩溃兜底写日志 (实现在 runtime/lifecycle/startup.c)。
 *
 * 与 SDL_Log 的区别: 不经过日志过滤、不取日志互斥体、不分配内存, 直接往本次会话日志
 * 追加一行并 flush。它只会在未处理异常过滤器里被调用 —— 那时进程可能已经处于不一致
 * 状态, 任何"先加锁再格式化"的路径都可能自己再挂一次。
 * 日志路径还没解析出来时只写 stderr。
 */
void bongo_cat_runtime_log_crash_line(const char *text);

#endif
