/*
 * 自检第 1 组: 版本与系统。
 *
 * 这一组回答的是"报告里的其它数字是在什么前提下拿到的": 版本、这次构建有没有编入
 * Vulkan、系统与 DPI、包的形态、进程权限。排查 200% DPI 相关问题 (用户报的就是) 时,
 * "缩放"必须是一个确定值, 而不是让支持人员去猜。
 */
#include "selfcheck_internal.h"

#include "bongo_cat/app.h"
#include "bongo_cat/log.h"
#include "bongo_cat/platform.h"
#include "bongo_cat/render.h"
#include "ui_backend.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include "windows_diagnostics.h"
#include "windows_package.h"
#endif

static void describe_arch(char *output, size_t capacity) {
#ifdef _WIN32
    snprintf(output, capacity, "%s", bongo_cat_windows_selfcheck_arch());
#else
    snprintf(output, capacity, "%s",
        sizeof(void *) == 8 ? "64-bit" : "32-bit");
#endif
}

static void describe_os(char *output, size_t capacity) {
#ifdef _WIN32
    if (bongo_cat_windows_selfcheck_os(output, capacity)) return;
#endif
    /* 拿不到具体版本时给出 SDL 自报的平台名: 仍是明确结论, 不是"未知" */
    snprintf(output, capacity, "%s", SDL_GetPlatform());
}

void bongo_cat_selfcheck_env(BongoCatSelfCheckBuilder *builder,
    struct BongoCatApp *app) {
    bongo_cat_selfcheck_section(builder, "版本与系统");
    bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_PASS, "程序版本",
        "%s %s", BONGO_CAT_NAME, BONGO_CAT_VERSION);
    bool vulkan_built = bongo_cat_renderer_compiled_in(BONGO_CAT_RENDERER_VULKAN);
    bongo_cat_selfcheck_add(builder, vulkan_built ? BONGO_CAT_SELFCHECK_PASS :
        BONGO_CAT_SELFCHECK_WARN, "构建后端能力",
        vulkan_built
            ? "Vulkan 已编入本构建; 当前生效后端: %s"
            : "Vulkan 未编入本构建 (只有 OpenGL); 当前生效后端: %s",
        bongo_cat_renderer_name(bongo_cat_renderer_active()));
    char os[128] = {0}, arch[16] = {0};
    describe_os(os, sizeof(os));
    describe_arch(arch, sizeof(arch));
    float scale = 1.0f;
    int logical_width = 0, logical_height = 0, pixel_width = 0, pixel_height = 0;
    if (app && app->window) {
        /*
         * 这里过去把 SDL_GetWindowSize 直接当"逻辑尺寸"打印。在 Windows 的 DPI 感知
         * 窗口里那是物理像素 —— 200% 缩放下窗口 564x493 会打印成
         * "564x493 像素 (564x493 逻辑)", 与 [ui] 日志里同一窗口的
         * "逻辑=282x247 像素=564x493" 自相矛盾, 看上去像"HiDPI 没生效"。
         * 三个口径统一从 ui_scale.c 取, 与日志/布局/输入同源。
         */
        BongoCatUIWindowMetrics metrics;
        bongo_cat_ui_window_metrics(app->window, &metrics);
        scale = SDL_GetWindowDisplayScale(app->window);
        pixel_width = metrics.pixel_width;
        pixel_height = metrics.pixel_height;
        logical_width = metrics.logical_width;
        logical_height = metrics.logical_height;
    }
    bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_PASS, "操作系统",
        "%s · %s · DPI 缩放 %.0f%% (%.2fx) · 宠物窗口 %dx%d 像素 (%dx%d 逻辑)",
        os, arch, (double)(scale * 100.0f), (double)scale, pixel_width,
        pixel_height, logical_width, logical_height);
#ifdef _WIN32
    const bool packaged = bongo_cat_windows_is_packaged();
    bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_PASS, "包标识",
        "%s", packaged ? "MSIX 打包安装" : "unpackaged Win32 (绿色版/安装包)");
#else
    bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_PASS, "包标识",
        "unpackaged %s", SDL_GetPlatform());
#endif
    const bool elevated = bongo_cat_platform_is_elevated();
    bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_PASS, "进程权限",
        elevated
            ? "以管理员身份运行 (可捕获部分系统级按键)"
            : "普通权限运行 (部分全屏游戏里的按键可能收不到, 可开启"
              "「以管理员身份运行」)");
}
