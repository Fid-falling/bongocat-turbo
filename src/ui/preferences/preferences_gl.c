/*
 * 设置窗口的图形上下文: Vulkan 走共享设备, OpenGL 走"能共享就共享"。
 * 本文件里的所有失败路径都必须把主窗口的上下文设回当前 —— 设置窗口建上下文的
 * 过程会抢占 "current", 忘了还回去, 主窗口(宠物)下一帧就画不出来或画到错窗口上。
 */
#include "preferences_gl.h"
#include "preferences_state.h"
#include "bongo_cat/render.h"

#include <SDL3/SDL_opengl.h>
#include <stdio.h>

/*
 * 统一失败出口: 打一条带三个窗口/上下文指针的日志 (这类问题只能靠指针区分是哪个
 * 窗口出事), 把当前上下文还给主窗口, 再把错误串塞回 SDL 供上层记录。
 */
static bool fail(BongoCatPreferences *value, const char *message) {
    char detail[256];
    snprintf(detail, sizeof(detail), "%s",
        message && message[0] ? message : "unknown OpenGL error");
    SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
        "Preferences dedicated OpenGL context failed: %s "
        "main_window=%p settings_window=%p main_context=%p",
        detail, (void *)value->app->window, (void *)value->window,
        (void *)value->app->gl_context);
    /* 主窗口可能是 Vulkan 后端 (没有 GL 上下文), 这时不要把上下文设成 NULL。 */
    if (value->app->gl_context)
        SDL_GL_MakeCurrent(value->app->window, value->app->gl_context);
    SDL_SetError("%s", detail);
    return false;
}

bool bongo_cat_preferences_gl_create(BongoCatPreferences *value) {
    if (!value || !value->app || !value->window || !value->app->window)
        return false;
    /*
     * renderer=vulkan 且这个窗口允许时: 设置界面走自己的 Vulkan 上下文。
     * 注意窗口的图形 API 标志是在创建时按 prefer_vulkan 定下的
     * (preferences_scale.c), 而 prefer_vulkan 已经考虑了
     * BONGO_CAT_PREFERENCES_OPENGL 退回开关 —— 这里不能再改, 给一个
     * SDL_WINDOW_VULKAN 窗口建 GL 上下文会被 SDL 拒绝。
     */
    if (value->prefer_vulkan &&
        bongo_cat_renderer_active() == BONGO_CAT_RENDERER_VULKAN) {
        BongoCatError error = {0};
        /*
         * 共用主窗口的设备与队列 (只各建各的交换链)。之前这里建的是第二套
         * VkInstance/VkDevice, 在注入了 RTSS / OBS / NVIDIA 叠加层钩子的环境里
         * 第一帧呈现就会崩在 vkQueuePresentKHR 内部 (见 preferences_scale.c 的注释)。
         */
        value->vk = bongo_cat_vk_create_shared(value->window, value->app->vk, &error);
        if (!value->vk) {
            SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
                "Preferences Vulkan context failed: %s",
                error.message[0] ? error.message : "unknown");
            return false;
        }
        BongoCatVkInfo info = {0};
        if (bongo_cat_vk_info(value->vk, &info))
            SDL_Log("[runtime] Preferences Vulkan context: device=%s driver=%s "
                "swapchain=%dx%d", info.device, info.driver, info.width, info.height);
        /*
         * 设置界面是事件驱动渲染 (滚轮/拖动/动画一来就画一帧), 继承主窗口的 FIFO
         * 会让每次 vkQueuePresentKHR 都等一个垂直回扫 (>=16.7ms), 滚动就一顿一顿的;
         * 所以默认关掉同步, 呈现走 IMMEDIATE / MAILBOX。
         *
         * 配合 render() 里的 60 Hz 节流, present 次数本来就和刷新率对齐; 如果某个
         * 叠加层 (RTSS/小飞机 OSD 之类) 在 IMMEDIATE 下反而更卡, 可以用
         * BONGO_CAT_PREFS_VSYNC=1 切回 FIFO 对比 —— 不用重新编译。
         */
        const char *vsync_override = SDL_getenv("BONGO_CAT_PREFS_VSYNC");
        bongo_cat_vk_set_vsync(value->vk,
            vsync_override && vsync_override[0] == '1');
        return true;
    }
    /* 主窗口是 OpenGL 时共享它的上下文 (贴图互通、省显存); 主窗口是 Vulkan 时
       没有可共享的对象, 设置窗口自己建一个独立的上下文。 */
    const bool share = value->app->gl_context != NULL;
    if (share && !SDL_GL_MakeCurrent(value->app->window, value->app->gl_context))
        return fail(value, SDL_GetError());
    /* The transparent settings window relies on per-pixel alpha to clip the
       rounded corners (bongo_cat_ui_paint_rounded_surface). Request an alpha
       channel explicitly so the corner mask reaches the framebuffer. */
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    if (share && !SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1))
        return fail(value, SDL_GetError());
    /*
     * SDL_GL_*Attribute 是全局状态, 不是"这次创建"的参数:
     * - 共享属性会让 SDL_GL_CreateContext 去和"当前上下文"共享, 设完必须立刻
     * 复位, 否则之后任何地方新建的上下文都会莫名其妙地去共享同一个对象;
     * - 复位失败说明进程已经处在不可控状态, 这时宁可销毁刚建好的上下文也不
     * 继续 —— 留着它等于把一个会传染的全局状态带进后续所有创建流程。
     * 错误串要提前拷出来: SDL_SetError 的内容会被后续 SDL 调用覆盖。
     */
    SDL_GLContext context = SDL_GL_CreateContext(value->window);
    char creation_error[256] = {0};
    if (!context) snprintf(creation_error, sizeof(creation_error), "%s",
        SDL_GetError());
    bool reset = SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
    if (!context)
        return fail(value, creation_error[0] ? creation_error :
            "SDL_GL_CreateContext returned no context");
    if (!reset) {
        snprintf(creation_error, sizeof(creation_error), "%s", SDL_GetError());
        SDL_GL_DestroyContext(context);
        return fail(value, creation_error);
    }
    if (!SDL_GL_MakeCurrent(value->window, context)) {
        snprintf(creation_error, sizeof(creation_error), "%s", SDL_GetError());
        SDL_GL_DestroyContext(context);
        return fail(value, creation_error);
    }
    value->gl_context = context;
    value->owns_gl_context = true;
    const GLubyte *vendor = glGetString(GL_VENDOR);
    const GLubyte *renderer = glGetString(GL_RENDERER);
    const GLubyte *version = glGetString(GL_VERSION);
    SDL_Log("[runtime] Preferences OpenGL context: shared=1 dedicated=1 "
        "main_window=%p settings_window=%p main_context=%p settings_context=%p "
        "vendor=%s renderer=%s version=%s",
        (void *)value->app->window, (void *)value->window,
        (void *)value->app->gl_context, (void *)value->gl_context,
        vendor ? (const char *)vendor : "unknown",
        renderer ? (const char *)renderer : "unknown",
        version ? (const char *)version : "unknown");
    return true;
}

/*
 * 释放顺序: 先销毁设置窗口自己的 Vulkan 上下文 (共享设备时它只是交换链, 不会动
 * 主窗口的设备), 再处理 GL 上下文。销毁 GL 上下文之前必须先把它从 current 上摘
 * 下来, 否则驱动里会留下一个指向已销毁上下文的活动槽位。
 * 全程不回问 GL 驱动除非确实拥有 GL 上下文 (见下面的保护)。
 */
void bongo_cat_preferences_gl_destroy(BongoCatPreferences *value) {
    if (!value) return;
    if (value->vk) {
        bongo_cat_vk_destroy(value->vk);
        value->vk = NULL;
    }
    if (!value->gl_context) return;
    SDL_Log("[runtime] Preferences OpenGL context release: "
        "settings_window=%p settings_context=%p current_window=%p "
        "current_context=%p", (void *)value->window,
        (void *)value->gl_context,
        value->prefer_vulkan ? (void *)0 : (void *)SDL_GL_GetCurrentWindow(),
        value->prefer_vulkan ? (void *)0 : (void *)SDL_GL_GetCurrentContext());
    /*
     * SDL_GL_GetCurrentContext() 会去问 GL 驱动; 设置窗口在 Vulkan 模式下没有 GL
     * 上下文, 不该碰它 (WER 显示崩在 nvoglv64.dll, 0xc0000005)。
     */
    if (!value->prefer_vulkan && SDL_GL_GetCurrentContext() == value->gl_context) {
        /* 主窗口是 Vulkan 时没有 GL 上下文可切回, 直接解绑。 */
        SDL_GL_MakeCurrent(value->app->gl_context ? value->app->window : NULL,
            value->app->gl_context);
    }
    if (value->owns_gl_context) SDL_GL_DestroyContext(value->gl_context);
    value->gl_context = NULL;
    value->owns_gl_context = false;
}
