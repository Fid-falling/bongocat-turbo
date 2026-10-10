#include "preferences_gl.h"
#include "preferences_state.h"
#include "bongo_cat/render.h"

#include <SDL3/SDL_opengl.h>
#include <stdio.h>

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
    /* renderer=vulkan 且这个窗口允许时: 设置界面走自己的 Vulkan 上下文 */
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
         * 设置界面是事件驱动渲染 (滚轮/拖动一来就画一帧), 继承主窗口的 FIFO 会让每次
         * vkQueuePresentKHR 都等一个垂直回扫 (>=16.7ms), 滚动就一顿一顿的。这里关掉
         * 垂直同步, 呈现走 IMMEDIATE / MAILBOX —— 交换链此时还没建, 所以只改字段,
         * 真正的呈现模式在下一帧创建交换链时生效。
         */
        bongo_cat_vk_set_vsync(value->vk, false);
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
