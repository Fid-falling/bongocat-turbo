#include "windows_layered_internal.h"
#include "windows_capture.h"

#ifdef _WIN32
#include <SDL3/SDL.h>
#include <SDL3/SDL_properties.h>
#include <windows.h>

/*
 * 分层窗口模式下的窗口级状态设置(不透明度 / 逐像素 alpha 查询 / 显示隐藏),
 * 也就是 platform.h 上那几个 set_* 的 Windows 实现。
 *
 * 平台层始终有两条路径: 走分层窗口(active)时透明度由我们自己合成进像素, 否则
 * 退回 SDL 的整窗不透明度。每个函数都要照顾到两条路径, 漏一条就会在某些配置下
 * 静默失效。
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
    bool updated;
    /* 已经提交过、而且 alpha 和上次相同, 就不必再打扰 DWM: 设置界面拖动滑块时会
       反复调用这里, 每次都重传整帧会明显卡顿。 */
    if (value && value->active) {
        BYTE alpha = (BYTE)(platform->window_opacity * 255.0f + 0.5f);
        updated = !value->has_frame ||
            (value->applied_alpha_valid && value->applied_alpha == alpha) ||
            bongo_cat_windows_layered_update_proxy(platform, value);
    } else {
        updated = SDL_SetWindowOpacity(platform->window, platform->window_opacity);
    }
    if (!updated) platform->window_opacity = previous;
    return updated;
}

/*
 * 取某个像素的 alpha, 供点击穿透的命中测试使用(见 windows_pointer.c)。
 * 优先用 readback(最近一次真正提交出去的内容), 只有它还没准备好时才退回 pixels
 * —— "刚写完但还没提交"的那一刻两者是不同的。
 * 尺寸不匹配直接返回 false: 拿旧尺寸的 alpha 会算出错误的穿透区域。
 */
bool bongo_cat_platform_frame_alpha(const BongoCatPlatform *platform,
    int width, int height, int x, int y, uint8_t *alpha) {
    const BongoCatWindowsLayered *value = platform ? platform->presenter : NULL;
    const unsigned char *pixels = value && value->readback_valid ?
        value->readback : value ? value->pixels : NULL;
    if (!value || !value->active || !value->has_frame || !pixels ||
        value->source_width != width || value->source_height != height ||
        !alpha || x < 0 || y < 0 || x >= width || y >= height)
        return false;
    *alpha = pixels[((size_t)y * (size_t)width + (size_t)x) * 4 + 3];
    return true;
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
