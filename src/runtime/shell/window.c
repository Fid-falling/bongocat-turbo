#include "runtime.h"
#include "bongo_cat/log.h"
#include "bongo_cat/render.h"
#include "bongo_cat/vk_backend.h"
#include "bongo_cat/live2d_vulkan.h"
#include <SDL3/SDL_opengl.h>
#include <stdio.h>

static bool test_transparency_disabled(void) {
    return SDL_getenv("BONGO_CAT_TEST_DISABLE_PREFERENCES_TRANSPARENCY") != NULL;
}

/*
 * Vulkan 主窗口。与 OpenGL 路径的区别:
 *  - 窗口带 SDL_WINDOW_VULKAN, 不设置任何 GL 属性;
 *  - 没有 MSAA 重试循环, 交换链的取样设置在渲染通道里;
 *  - 失败时把原因写进 error, 调用方切回 OpenGL 再试一次。
 */
static bool try_vulkan_window(BongoCatApp *app, BongoCatError *error) {
    /* 与 BONGO_CAT_TEST_GL_FALLBACK 对应: 让 Vulkan 初始化必定失败, 用于验证
       "失败也要能起来" 的回退路径。 */
    if (SDL_getenv("BONGO_CAT_TEST_VULKAN_FALLBACK")) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "test hook BONGO_CAT_TEST_VULKAN_FALLBACK");
        return false;
    }
#if defined(BONGO_CAT_HAS_CUBISM)
    /*
     * Live2D 的 Vulkan 渲染器按相对路径打开 SDK 的 FrameworkShaders/*.spv。
     * 用户经常只把 exe 复制到别处 (比如桌面) 就跑: 那时 exe 旁边没有这些文件,
     * 模型会画成一小块破碎的画面。这里提前判断, 找不到就明确失败并让上层回退
     * OpenGL —— 与其画坏, 不如用能正常工作的后端。
     */
    bongo_cat_live2d_vulkan_set_shader_root(app->asset_root);
    if (!bongo_cat_live2d_vulkan_shaders_available(app->asset_root)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "缺少 Live2D 的 Vulkan 着色器 (FrameworkShaders/*.spv): 请把 Release "
            "目录里的 FrameworkShaders 文件夹和 exe 放在一起");
        return false;
    }
#endif
    bool transparent = !test_transparency_disabled();
    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_BORDERLESS |
        SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
    if (transparent) flags |= SDL_WINDOW_TRANSPARENT;
    app->window = SDL_CreateWindow(BONGO_CAT_PET_WINDOW_TITLE,
        app->session.window.width, app->session.window.height, flags);
    if (!app->window) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Vulkan 窗口创建失败: %s", SDL_GetError());
        return false;
    }
    app->vk = bongo_cat_vk_create(app->window, error);
    if (!app->vk) {
        if (!error->message[0])
            bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
                "Vulkan 后端初始化失败");
        SDL_DestroyWindow(app->window);
        app->window = NULL;
        return false;
    }
    BongoCatVkInfo info = {0};
    /*
     * 只记下后端与尺寸并打开渲染器工厂开关 —— 不调用任何 SDK 函数, 因为 SDK 的
     * csmMap 依赖 CubismFramework::StartUp 设置的全局分配器, 早于它调用会崩。
     * 真正的挂载由模型在创建渲染器时完成 (那时框架已经启动)。
     */
    {
        BongoCatVkInfo remember_info = {0};
        if (bongo_cat_vk_info(app->vk, &remember_info) &&
            !bongo_cat_live2d_vulkan_remember(app->vk, remember_info.width,
                remember_info.height))
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                "[vulkan] Live2D 的 Vulkan 渲染器未登记, Live2D 将回退 OpenGL");
    }

    if (bongo_cat_vk_info(app->vk, &info))
        SDL_Log("[runtime] Vulkan 窗口就绪 (transparent=%d, device=%s, driver=%s, "
            "api=%s, swapchain=%dx%d)", transparent ? 1 : 0, info.device,
            info.driver, info.api, info.width, info.height);
    return true;
}

static bool set_gl_attributes(int samples) {
    SDL_GL_ResetAttributes();
#ifdef __APPLE__
    const int major = 4, minor = 1, profile = SDL_GL_CONTEXT_PROFILE_CORE;
#elif defined(BONGO_CAT_HAS_CUBISM)
    const int major = 3, minor = 3, profile = SDL_GL_CONTEXT_PROFILE_COMPATIBILITY;
#else
    const int major = 3, minor = 3, profile = SDL_GL_CONTEXT_PROFILE_CORE;
#endif
    return SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major) &&
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor) &&
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, profile) &&
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1) &&
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0) &&
        SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8) &&
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, samples > 0 ? 1 : 0) &&
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, samples);
}

static bool try_window(BongoCatApp *app, bool transparent, int samples,
    char *failure, size_t capacity) {
    if (test_transparency_disabled())
        transparent = false;
    if (!set_gl_attributes(samples)) {
        snprintf(failure, capacity, "OpenGL attributes: %s", SDL_GetError()); return false;
    }
    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_BORDERLESS |
        SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
    if (transparent) flags |= SDL_WINDOW_TRANSPARENT;
    app->window = SDL_CreateWindow(BONGO_CAT_PET_WINDOW_TITLE,
        app->session.window.width,
        app->session.window.height, flags);
    if (!app->window) {
        snprintf(failure, capacity, "Window creation: %s", SDL_GetError()); return false;
    }
    app->gl_context = SDL_GL_CreateContext(app->window);
    if (!app->gl_context || !SDL_GL_MakeCurrent(app->window, app->gl_context)) {
        snprintf(failure, capacity, "OpenGL context: %s", SDL_GetError());
        if (app->gl_context) SDL_GL_DestroyContext(app->gl_context);
        SDL_DestroyWindow(app->window); app->gl_context = NULL; app->window = NULL;
        return false;
    }
    return true;
}

BongoCatResult bongo_cat_window_create(BongoCatApp *app, BongoCatError *error) {
    SDL_SetHintWithPriority(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1", SDL_HINT_OVERRIDE);
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
#ifdef _WIN32
    /* The native input thread owns keyboard Raw Input for this process. */
    SDL_SetHintWithPriority(SDL_HINT_WINDOWS_RAW_KEYBOARD, "0", SDL_HINT_OVERRIDE);
    /* Transparent OpenGL windows must never receive SDL's default black
       WM_ERASEBKGND fill before the first frame is submitted. */
    SDL_SetHint(SDL_HINT_WINDOWS_ERASE_BACKGROUND_MODE, "0");
#endif
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "SDL initialization failed: %s", SDL_GetError());
        return BONGO_CAT_ERROR_PLATFORM;
    }
    /* 选定渲染后端。Vulkan 可用时按 SDL_WINDOW_VULKAN 建窗并初始化设备;
       初始化失败 (驱动/设备被占用等) 会在下面回退到 OpenGL 重试一次。 */
    bongo_cat_renderer_select(app->settings.model.renderer);
    if (bongo_cat_renderer_active() == BONGO_CAT_RENDERER_VULKAN) {
        BongoCatError vk_error = {0};
        if (try_vulkan_window(app, &vk_error)) return BONGO_CAT_OK;
        bongo_cat_renderer_fallback(BONGO_CAT_RENDERER_VULKAN,
            vk_error.message);
    }
    /* Keep a lower-cost MSAA path for drivers that cannot provide 4 samples. */
    const int options[][2] = {{true, 4}, {true, 2}, {true, 0}, {false, 0}};
    char failure[256] = {0};
    bool force_fallback = SDL_getenv("BONGO_CAT_TEST_GL_FALLBACK") != NULL;
    for (size_t i = 0; i < sizeof(options) / sizeof(options[0]); ++i) {
        if (force_fallback && options[i][1] > 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "Test requested OpenGL fallback"); continue;
        }
        if (try_window(app, options[i][0], options[i][1], failure, sizeof(failure))) {
            int sample_buffers = 0, sample_count = 0;
            SDL_GL_GetAttribute(SDL_GL_MULTISAMPLEBUFFERS, &sample_buffers);
            SDL_GL_GetAttribute(SDL_GL_MULTISAMPLESAMPLES, &sample_count);
            const GLubyte *vendor = glGetString(GL_VENDOR);
            const GLubyte *renderer = glGetString(GL_RENDERER);
            const GLubyte *version = glGetString(GL_VERSION);
            SDL_Log("[runtime] OpenGL window ready (transparent=%d, MSAA=%d, "
                "sample_buffers=%d, sample_count=%d)", options[i][0], options[i][1],
                sample_buffers, sample_count);
            SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
                "[runtime] OpenGL 上下文: vendor=%s renderer=%s version=%s MSAA=%d/%d",
                vendor ? (const char *)vendor : "unknown",
                renderer ? (const char *)renderer : "unknown",
                version ? (const char *)version : "unknown",
                sample_buffers, sample_count);
            {
                GLint max_texture = 0;
                glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture);
                SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
                    "[runtime] OpenGL 限制: max_texture=%d", (int)max_texture);
            }
            if (!SDL_GL_SetSwapInterval(0)) SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                "Vertical sync could not be disabled: %s", SDL_GetError());
            return BONGO_CAT_OK;
        }
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "OpenGL attempt %llu failed: %s",
            (unsigned long long)(i + 1), failure);
    }
    bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
        "Window and OpenGL initialization failed after compatibility retries: %s", failure);
    return BONGO_CAT_ERROR_PLATFORM;
}

void bongo_cat_window_apply(BongoCatApp *app) {
    BongoCatWindowPreferences *preferences = &app->settings.window;
    BongoCatWindowState *state = &app->session.window;
    bongo_cat_app_cancel_hover_fade(app);
    bongo_cat_platform_set_opacity(&app->platform,
        state->opacity_percent / 100.0f);
    SDL_SetWindowSize(app->window, state->width, state->height);
    if (state->position_known)
        SDL_SetWindowPosition(app->window, state->x, state->y);
    SDL_SyncWindow(app->window);
    if (preferences->keep_in_screen) bongo_cat_window_clamp_to_display(app);
    else bongo_cat_window_recover_to_display(app);
    SDL_SyncWindow(app->window);
    /* A visible session is revealed by the first successful frame. Keeping
       the native window hidden while loading avoids exposing an uninitialised
       (and on some drivers black) back buffer. */
    bongo_cat_platform_set_visible(&app->platform,
        state->visible && !app->startup_visibility_pending);
    bongo_cat_window_sync_click_through(app);
    bongo_cat_platform_set_always_on_top(&app->platform,
        preferences->always_on_top);
    /* 只在录屏软件里显示 (Windows: DWM 隐藏)。放在最后: 上面几个调用都会动窗口
       样式/位置, 重新应用一次能保证隐藏状态不会在它们之后丢失。 */
    bongo_cat_platform_set_capture_only(&app->platform,
        app->settings.window.capture_only);
}

void bongo_cat_window_apply_capture_only(BongoCatApp *app) {
    if (!app || !app->window) return;
    if (!bongo_cat_platform_capture_only_supported()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "仅在录屏软件中显示在当前平台不可用, 该设置已忽略");
        return;
    }
    if (!bongo_cat_platform_set_capture_only(&app->platform,
            app->settings.window.capture_only)) return;
    /* 桌面不可见时点击穿透必须整体生效: 让下一帧重新下发一次点击穿透状态
       (平台侧在 capture_only 打开时会强制整体穿透, 不再做逐像素命中测试)。 */
    app->click_through_valid = false;
    bongo_cat_window_sync_click_through(app);
    app->dirty = true;
}

static bool event_targets_main_window(BongoCatApp *app,
    const SDL_Event *event) {
    SDL_WindowID id = SDL_GetWindowID(app->window);
    if (event->type >= SDL_EVENT_WINDOW_FIRST &&
        event->type <= SDL_EVENT_WINDOW_LAST)
        return event->window.windowID == id;
    switch (event->type) {
    case SDL_EVENT_MOUSE_MOTION:
        return event->motion.windowID == id || app->window_drag_active ||
            app->drag_candidate || app->resize_gesture;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        return event->button.windowID == id;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        return event->button.windowID == id || app->window_drag_active ||
            app->drag_candidate || app->resize_gesture;
    case SDL_EVENT_MOUSE_WHEEL:
        return event->wheel.windowID == id;
    default:
        return true;
    }
}

bool bongo_cat_window_event(BongoCatApp *app, const SDL_Event *event) {
    bongo_cat_window_display_event(app, event);
    if (!event_targets_main_window(app, event)) return true;
    if (event->type == SDL_EVENT_QUIT) return false;
    if (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        bongo_cat_window_set_visible(app, false);
        return true;
    }
    if (event->type == SDL_EVENT_WINDOW_HIDDEN) {
        bongo_cat_window_drag_end(app);
    }
    if (event->type == SDL_EVENT_WINDOW_MINIMIZED) {
        app->window_minimized = true;
        bongo_cat_window_drag_end(app);
    }
    if (event->type == SDL_EVENT_WINDOW_RESIZED) {
        /* Queued notifications may describe an earlier animation frame. */
        int width = app->session.window.width, height = app->session.window.height;
        SDL_GetWindowSize(app->window, &width, &height);
        if (width != app->session.window.width || height != app->session.window.height) {
            app->session.window.width = width;
            app->session.window.height = height;
            bongo_cat_window_content_size(app, width,
                height, &app->session.window.content_width,
                &app->session.window.content_height);
            bongo_cat_window_clamp_to_display(app);
            app->dirty = true;
        }
    }
    if (event->type == SDL_EVENT_WINDOW_EXPOSED ||
        event->type == SDL_EVENT_WINDOW_SHOWN ||
        event->type == SDL_EVENT_WINDOW_RESTORED) {
        /* An expose can arrive without a restore on XWayland, but a queued
           expose is not proof that the window is currently restored. */
        app->window_minimized =
            (SDL_GetWindowFlags(app->window) & SDL_WINDOW_MINIMIZED) != 0;
        if (event->type != SDL_EVENT_WINDOW_EXPOSED)
            bongo_cat_app_reset_pointer_tracking(app);
        /* DWM can discard the transparent redirection surface after an
           Explorer/display refresh. Repaint even when the model is idle so
           the restored alpha surface is submitted immediately. */
        app->dirty = true;
    }
    if (event->type == SDL_EVENT_WINDOW_FOCUS_GAINED ||
        event->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        bongo_cat_app_reset_pointer_tracking(app);
    }
    if (event->type == SDL_EVENT_WINDOW_RESIZED ||
        event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
        int width = 0, height = 0;
        if (SDL_GetWindowSizeInPixels(app->window, &width, &height) &&
            (width != app->resize_pixel_width || height != app->resize_pixel_height)) {
            app->resize_pixel_width = width;
            app->resize_pixel_height = height;
            app->resize_pending = true;
            app->dirty = true;
            bongo_cat_window_mark_hit_dirty(app);
        }
    } else if (event->type == SDL_EVENT_WINDOW_MOVED) {
        int x = 0, y = 0;
        if (SDL_GetWindowPosition(app->window, &x, &y) &&
            (!app->session.window.position_known ||
             x != app->session.window.x || y != app->session.window.y)) {
            app->session.window.x = x;
            app->session.window.y = y;
            app->session.window.position_known = true;
            app->pointer_known = false;
            if (!app->window_drag_active) bongo_cat_window_clamp_to_display(app);
            bongo_cat_window_mark_hit_dirty(app);
        }
    } else if (event->type == SDL_EVENT_WINDOW_DISPLAY_CHANGED ||
        event->type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
        bongo_cat_app_reset_pointer_tracking(app);
    } else if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        event->button.button == SDL_BUTTON_LEFT) {
        bongo_cat_window_drag_begin(app, &event->button);
    } else if (event->type == SDL_EVENT_MOUSE_MOTION) {
        bongo_cat_window_resize_by_pointer(app, event);
        bongo_cat_window_drag_motion(app, &event->motion);
    } else if (event->type == SDL_EVENT_MOUSE_WHEEL) {
        bongo_cat_window_wheel(app, &event->wheel);
    } else if (event->type == SDL_EVENT_MOUSE_BUTTON_UP &&
        event->button.button == SDL_BUTTON_LEFT) {
        bongo_cat_window_mark_hit_dirty(app);
        bongo_cat_window_drag_end(app);
    } else if (event->type == SDL_EVENT_MOUSE_BUTTON_UP &&
        event->button.button == SDL_BUTTON_RIGHT) {
        bongo_cat_window_mark_hit_dirty(app);
        if (app->resize_gesture) app->resize_gesture = false;
        else bongo_cat_window_show_context_menu(app);
    }
    return true;
}

void bongo_cat_window_destroy(BongoCatApp *app) {
    bongo_cat_window_drag_end(app);
    if (app->vk) {
        bongo_cat_vk_destroy(app->vk);
        app->vk = NULL;
    }
    if (app->gl_context && SDL_GL_MakeCurrent(app->window, app->gl_context))
        bongo_cat_window_destroy_corner_mask();
    if (app->gl_context) SDL_GL_DestroyContext(app->gl_context);
    if (app->window) SDL_DestroyWindow(app->window);
    app->gl_context = NULL;
    app->window = NULL;
    SDL_Quit();
}
