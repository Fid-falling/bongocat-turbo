#include "bongo_cat/render.h"
#include "bongo_cat/log.h"
#include "bongo_cat/vk_backend.h"

#include <stdio.h>

/*
 * 日志类别说明: 项目的日志策略 (bongo_cat/log.h) 只对 LIFECYCLE/UPDATE/INPUT
 * 三个类别放行 INFO, 其余类别只有 WARN 以上才会输出。所以后端选定的信息行
 * 必须挂在 BONGO_CAT_LOG_LIFECYCLE 上, 否则用户根本看不到实际用的是哪个后端。
 *
 * Vulkan 后端的实现进度开关。
 * 0 = 只有编译期能力 (设置里能选, 运行时回退 OpenGL);
 * 1 = 设备/交换链/主窗口绘制已经落地, 选中即真正生效。
 */
#define BONGO_CAT_VULKAN_BACKEND_IMPLEMENTED 1

static BongoCatRenderer active_renderer = BONGO_CAT_RENDERER_OPENGL;
/* 探测失败原因需要活到调用方读完, 所以放在静态缓冲里。 */
static char vk_unavailable_reason[256];

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
#if !BONGO_CAT_VULKAN_BACKEND_IMPLEMENTED
        return "Vulkan 后端尚未实现, 目前只有 OpenGL 可用";
#elif defined(BONGO_CAT_HAS_CUBISM)
        /* Cubism 模型的渲染器仍然是 OpenGL 实现, 在它移植完成之前, 让带
           Cubism 的构建明确地留在 OpenGL 上, 而不是画出一个没有模型的窗口。 */
        /*
         * 带 Cubism 的构建现在也能用 Vulkan: Live2D 走 SDK 自带的
         * CubismRenderer_Vulkan, 先渲染到离屏纹理, 再由 Vulkan 后端的合成
         * 路径画进宠物窗口 (src/live2d/cubism_vulkan_bridge.cpp)。
         * 返回 NULL = 没有不可用原因, 即允许选择这个后端。
         */
        return NULL;
#else
        {
            BongoCatError probe = {0};
            if (bongo_cat_vk_probe(&probe)) return NULL;
            snprintf(vk_unavailable_reason, sizeof(vk_unavailable_reason), "%s",
                probe.message[0] ? probe.message : "Vulkan 设备不可用");
            return vk_unavailable_reason;
        }
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
    /*
     * AUTO 优先 Vulkan, 探测不通过 (没有设备/驱动太旧) 才回退 OpenGL。
     *
     * Vulkan 后端已经覆盖 OpenGL 的全部路径: 透明合成与预乘 alpha、2x 超采样
     * 抗锯齿、设置界面的图标/logo 与轮盘字形、模型封面与冒烟诊断的整帧回读。
     * 旧的 "auto" 配置会立刻跟着切过来。
     */
    if (requested == BONGO_CAT_RENDERER_AUTO)
        return bongo_cat_renderer_available(BONGO_CAT_RENDERER_VULKAN)
            ? BONGO_CAT_RENDERER_VULKAN : BONGO_CAT_RENDERER_OPENGL;
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

void bongo_cat_renderer_fallback(BongoCatRenderer failed, const char *reason) {
    if (failed != BONGO_CAT_RENDERER_VULKAN) return;
    active_renderer = BONGO_CAT_RENDERER_OPENGL;
    SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
        "[render] %s 初始化失败 (%s), 本次运行回退到 OpenGL",
        bongo_cat_renderer_name(failed), reason && reason[0] ? reason : "未知原因");
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
