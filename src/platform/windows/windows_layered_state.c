#include "windows_layered_internal.h"
#include "windows_capture.h"

#ifdef _WIN32
#include <SDL3/SDL.h>
#include <SDL3/SDL_properties.h>
#include <windows.h>

/*
 * 平台结构(BongoCatPlatform)与分层窗口呈现器之间的适配层, 也就是 platform.h 上那几个
 * 窗口级 set_* / 呈现入口的 Windows 实现。
 *
 * 为什么要有这一层: 呈现器本身是按窗口持有的实例(主窗口一份, 轮盘一份), 它不认识
 * BongoCatPlatform。主窗口把它放在 platform->presenter 上, 于是"平台的 window_opacity"
 * 与"实例的不透明度镜像"必须在这里保持同步 —— 两处不一致的表现是恢复窗口时突然变亮
 * 或变暗。轮盘不走这条路, 它自己 attach 实例(见 windows_layered_window.c)。
 *
 * 平台层始终有两条路径: 走分层窗口(active)时透明度由我们自己合成进像素, 否则退回
 * SDL 的整窗不透明度。每个函数都要照顾到两条路径, 漏一条就会在某些配置下静默失效。
 */
static HWND native_window(BongoCatPlatform *platform) {
    return platform && platform->window ? (HWND)SDL_GetPointerProperty(
        SDL_GetWindowProperties(platform->window),
        SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL) : NULL;
}

/*
 * 设置整窗不透明度。分层窗口模式下不是改窗口属性, 而是把 alpha 合成进下一次提交
 * 的像素(见 windows_layered.c 的预乘 alpha), 所以这里只负责触发重提交。
 * 返回值语义: false 表示这次设置没生效, 此时必须把 window_opacity 回滚 —— 这个
 * 字段同时也是别的模块读取的"当前值", 留着新值会让状态与画面不一致。
 */
bool bongo_cat_platform_set_opacity(BongoCatPlatform *platform,
    float opacity) {
    if (!platform || !platform->window) return false;
    BongoCatWindowsLayered *value = platform->presenter;
    float previous = platform->window_opacity;
    platform->window_opacity = SDL_clamp(opacity, 0.0f, 1.0f);
    /* 呈现器要自己拿这个值去合成像素与恢复窗口, 所以镜像必须跟着走(含下面的回滚)。 */
    if (value) value->opacity = platform->window_opacity;
    bool updated;
    /* 已经提交过、而且 alpha 和上次相同, 就不必再打扰 DWM: 设置界面拖动滑块时会
       反复调用这里, 每次都重传整帧会明显卡顿。 */
    if (value && value->active) {
        BYTE alpha = (BYTE)(platform->window_opacity * 255.0f + 0.5f);
        updated = !value->has_frame ||
            (value->applied_alpha_valid && value->applied_alpha == alpha) ||
            bongo_cat_windows_layered_update_proxy(value);
    } else {
        updated = SDL_SetWindowOpacity(platform->window, platform->window_opacity);
    }
    if (!updated) {
        platform->window_opacity = previous;
        if (value) value->opacity = previous;
    }
    return updated;
}

/*
 * 取某个像素的 alpha, 供点击穿透的命中测试使用(见 windows_pointer.c)。
 * 判定细节在 bongo_cat_windows_layered_frame_alpha 里(它优先读 readback: 最近一次
 * 真正提交出去的内容, 而不是刚写完还没提交的 pixels), 这里只做平台侧的取用。
 */
bool bongo_cat_platform_frame_alpha(const BongoCatPlatform *platform,
    int width, int height, int x, int y, uint8_t *alpha) {
    return bongo_cat_windows_layered_frame_alpha(
        platform ? platform->presenter : NULL, width, height, x, y, alpha);
}

/*
 * 每帧呈现。实现在 windows_layered.c —— 未启用代理模式时它就是普通的
 * SDL_GL_SwapWindow, 所以这里只负责把 platform 上的实例取出来。
 */
bool bongo_cat_platform_present(BongoCatPlatform *platform, int width, int height) {
    if (!platform || !platform->window) return false;
    if (!platform->presenter) return SDL_GL_SwapWindow(platform->window);
    return bongo_cat_windows_layered_present(platform->presenter, width, height);
}

/* Vulkan 后端是否走代理分层 (Vulkan 整帧回读 -> submit_rgba)。
   由 vk_scene.c 决定"要不要每帧排一次整帧回读": 只有真开着才排队, 免得普通
   Vulkan 模式被整帧回读拖慢。 */
bool bongo_cat_platform_layered_active(BongoCatPlatform *platform) {
    BongoCatWindowsLayered *value = platform ? platform->presenter : NULL;
    return value && value->active;
}

/* Vulkan 后端把整帧回读 (RGBA) 喂给代理分层呈现。窗口/呈现器语义全部交给
   windows_layered.c 的 submit_rgba; 这里是平台适配层, 没有代理模式时直接返回 false。 */
bool bongo_cat_platform_submit_layered_rgba(BongoCatPlatform *platform,
    int width, int height, const unsigned char *rgba) {
    if (!platform || !platform->presenter) return false;
    return bongo_cat_windows_layered_submit_rgba(platform->presenter,
        width, height, rgba);
}

/*
 * 显示 / 隐藏。隐藏时连代理窗口一起藏起来, 否则会出现"猫看不见了, 但还能点到
 * 一块区域"。
 * 重新显示后要重新配置采集属性并修复透明: 这段时间里 DWM 的合成状态和窗口的
 * 采集属性都可能被系统重置过(改分辨率、锁屏解锁、显示器热插拔)。
 */
void bongo_cat_platform_set_visible(BongoCatPlatform *platform,
    bool visible) {
    if (!platform || !platform->window) return;
    BongoCatWindowsLayered *value = platform->presenter;
    if (value) value->visible = visible;
    if (!visible && value && value->proxy) ShowWindow(value->proxy, SW_HIDE);
    if (visible) SDL_ShowWindow(platform->window);
    else SDL_HideWindow(platform->window);
    if (visible) {
        HWND source = native_window(platform);
        bongo_cat_windows_capture_configure(source);
        bongo_cat_windows_capture_repair_transparency(source);
        bongo_cat_windows_capture_log(source, "visible");
    }
}
#endif
