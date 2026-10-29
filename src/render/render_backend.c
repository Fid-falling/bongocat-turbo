/*
 * 后端选择。AUTO 现在解析成 Vulkan, 只有探针失败才退回 OpenGL: Vulkan 这一侧的功能
 * 已经对齐 (轮盘文字/模型封面/冒烟审计/整帧回读), 上层代码两个后端都能跑。
 *
 * 排查"画面不对"时先看启动日志里的 "[render] 请求后端=… 实际使用=…", 别靠猜 ——
 * Windows 10 上遇到不支持的能力 (例如交换链只报 OPAQUE) 时很容易静默退回。
 */

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
#else
        /*
         * 可用性问的是"这台机器 + 这次构建能不能跑起来", **不是**"现在是不是已经在用
         * Vulkan" —— 所以这里必须无条件探测, 与当前后端无关, 也不能因为"Live2D 的
         * Cubism 渲染器以前只有 OpenGL 版"就跳过探测:
         *   - 跳过探测 (以前带 Cubism 的构建就是直接 return NULL) 会让一台没有 Vulkan
         *     驱动的机器也把 Vulkan 段画成可选, 用户点了、写进配置、重启, 结果只是
         *     一次"回退到 OpenGL"的日志和一个没变化的界面;
         *   - 反过来, 探测里依赖"当前已有 Vulkan 实例/设备"的实现会把自己挡在门外
         *     (OpenGL 模式下永远探测失败), 那正是"设置里点了没反应"的来源。
         * 带 Cubism 的构建早就能走 Vulkan (CubismRenderer_Vulkan + 离屏合成, 见
         * src/live2d/cubism_vulkan_bridge.cpp), 所以探测结果就是唯一判据。
         */
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

void bongo_cat_renderer_select_source(BongoCatRenderer requested,
    BongoCatRenderer configured, bool command_line) {
    /*
     * 已经**明确要求** Vulkan (设置里选的, 或命令行 --vulkan) 时不再先探测一次可用性:
     * 探测本身要真建一套 VkInstance + VkDevice 再销毁 (实测 325ms), 紧接着建窗口又建一套
     * —— 同一件事做两遍, 那是启动路径上白花的四分之一秒。
     * 探测存在的意义是"在不动真格的情况下判断行不行", 对 AUTO 有意义 (它要据此选后端);
     * 用户已经点名要 Vulkan 时, 真正建一次窗口就是最准的判断, 而失败那条路本来就有:
     * 调用方 try_vulkan_window 失败会走 bongo_cat_renderer_fallback + 失败记录 + 可见提示。
     */
    if (requested == BONGO_CAT_RENDERER_VULKAN &&
        bongo_cat_renderer_compiled_in(BONGO_CAT_RENDERER_VULKAN))
        active_renderer = requested;
    else bongo_cat_renderer_select(requested);
    /*
     * 来源单独一行: 命令行覆盖 (--renderer=/--opengl/--vulkan) 的优先级高于
     * settings.json, 而且**不写回**配置。用户看到"设置里明明是 OpenGL 却在跑
     * Vulkan"时, 这一行就是答案。
     */
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[render] 后端来源=%s 配置=%s 请求=%s",
        command_line ? "命令行" : "settings.json",
        bongo_cat_renderer_name(configured),
        bongo_cat_renderer_name(requested));
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
