#include "bongo_cat/render.h"
#include "bongo_cat/log.h"

/*
 * 日志类别说明: 项目的日志策略 (bongo_cat/log.h) 只对 LIFECYCLE/UPDATE/INPUT
 * 三个类别放行 INFO, 其余类别只有 WARN 以上才会输出。所以后端选定的信息行
 * 必须挂在 BONGO_CAT_LOG_LIFECYCLE 上, 否则用户根本看不到实际用的是哪个后端。
 *
 * Vulkan 后端的实现进度开关。
 * BONGO_CAT_HAS_VULKAN 只说明"这次构建带了 Vulkan 的编译期能力";
 * 渲染器本身 (交换链/管线/同步) 属于后续阶段, 在它落地之前这里保持 0,
 * 免得设置里选了 Vulkan 之后启动到一半失败。
 */
#define BONGO_CAT_VULKAN_BACKEND_IMPLEMENTED 0

static BongoCatRenderer active_renderer = BONGO_CAT_RENDERER_OPENGL;

bool bongo_cat_renderer_compiled_in(BongoCatRenderer value) {
    switch (value) {
    case BONGO_CAT_RENDERER_OPENGL:
        return true;
    case BONGO_CAT_RENDERER_VULKAN:
#ifdef BONGO_CAT_HAS_VULKAN
        return true;
#else
        return false;
#endif
    case BONGO_CAT_RENDERER_AUTO:
    case BONGO_CAT_RENDERER_COUNT:
    default:
        return false;
    }
}

const char *bongo_cat_renderer_unavailable_reason(BongoCatRenderer value) {
    switch (value) {
    case BONGO_CAT_RENDERER_OPENGL:
        return NULL;
    case BONGO_CAT_RENDERER_VULKAN:
        if (!bongo_cat_renderer_compiled_in(value))
            return "此构建未启用 Vulkan (需要 -DBONGO_CAT_ENABLE_VULKAN=ON)";
#if BONGO_CAT_VULKAN_BACKEND_IMPLEMENTED
        return NULL;
#else
        return "Vulkan 后端尚未实现, 目前只有 OpenGL 可用";
#endif
    case BONGO_CAT_RENDERER_AUTO:
        return NULL; /* AUTO 本身不可选, 由 resolve 展开 */
    default:
        return "未知的渲染后端";
    }
}

bool bongo_cat_renderer_available(BongoCatRenderer value) {
    if (value == BONGO_CAT_RENDERER_AUTO) return false;
    return bongo_cat_renderer_unavailable_reason(value) == NULL;
}

BongoCatRenderer bongo_cat_renderer_resolve(BongoCatRenderer requested) {
    if (requested == BONGO_CAT_RENDERER_AUTO) return BONGO_CAT_RENDERER_OPENGL;
    if (bongo_cat_renderer_available(requested)) return requested;
    SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
        "[render] 请求的后端 %s 不可用 (%s), 回退到 OpenGL",
        bongo_cat_renderer_name(requested),
        bongo_cat_renderer_unavailable_reason(requested));
    return BONGO_CAT_RENDERER_OPENGL;
}

void bongo_cat_renderer_select(BongoCatRenderer requested) {
    active_renderer = bongo_cat_renderer_resolve(requested);
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[render] 请求后端=%s  实际使用=%s  本次构建 Vulkan=%s  后端已实现=%d",
        bongo_cat_renderer_name(requested),
        bongo_cat_renderer_name(active_renderer),
        bongo_cat_renderer_compiled_in(BONGO_CAT_RENDERER_VULKAN) ? "是" : "否",
        BONGO_CAT_VULKAN_BACKEND_IMPLEMENTED);
}

BongoCatRenderer bongo_cat_renderer_active(void) {
    return active_renderer;
}

const char *bongo_cat_renderer_description(BongoCatRenderer value) {
    switch (value) {
    case BONGO_CAT_RENDERER_AUTO:   return "Auto";
    case BONGO_CAT_RENDERER_OPENGL: return "OpenGL";
    case BONGO_CAT_RENDERER_VULKAN: return "Vulkan";
    default:                        return "Unknown";
    }
}
