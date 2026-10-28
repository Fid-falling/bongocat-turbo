/*
 * 首帧看门狗。
 *
 * 存在理由 (真实故障): v1.17.5 在 AMD Radeon 780M + 200% DPI + 要求 Vulkan 的机器上,
 * 日志停在 "[live2d/vk] create_renderer: uses_vulkan=1 bridge_ready=1" 之后再无输出,
 * 猫一直不出来, 用户在任务管理器看到进程"闪一下就消失"。那一段几乎没有任何日志, 用户
 * 也没有任何可操作的动作 —— 只能重装/换版本。
 *
 * 看门狗把这段空白变成一个确定的结论与一个确定的自救动作:
 *   启动后 8 秒内没看到"首帧真的端出去", 就打一条 ERROR (后端/GPU/驱动/DPI/交换链参数/
 *   当前阶段), 然后把渲染后端写回 OpenGL 并自动重启一次 —— 复用既有的重启机制
 *   (application_restart.c: 先落盘、再还单实例锁、主循环末尾拉起新进程)。
 *
 * 三个不变量:
 *   1. 只在**启动后**计时: arm 在装配完成后调用, 不把模型加载的几秒算进去;
 *   2. "成功呈现"的判据是 bongo_cat_startup_ready() 被调用 (它只在第一帧真的端出去或
 *      窗口本来就不显示时被调), 不是"初始化返回了 true";
 *   3. 同一进程只回退一次。回退会把 settings 里写死 OpenGL, 所以下一次启动不会再选中
 *      Vulkan; 即便 OpenGL 也端不出帧, 看门狗也只会再打一条 ERROR, 不会再重启 —— 否则
 *      就是无限重启循环。
 */
#include "runtime.h"

#include "bongo_cat/log.h"
#include "bongo_cat/render.h"
#include "bongo_cat/vk_backend.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

/*
 * 8 秒这个数字的来源: 冷启动 (含模型加载/Live2D Core 装载/首次建交换链) 在这台开发机上
 * 约 1.5-3 秒; 慢机器 + 大模型 + 首次加载着色器可以到 5-6 秒。8 秒给了足够余量, 又不会
 * 让用户对着不出现的猫等太久。测试钩子可以把判定提前, 但默认值不跟着改。
 */
#define BONGO_CAT_FIRST_FRAME_TIMEOUT_NS 8000000000ull

static uint64_t started_ns;
static bool armed;
static bool presented;
static bool fell_back;
static const char *current_stage = "startup";

void bongo_cat_first_frame_stage(const char *stage) {
    if (stage && stage[0]) current_stage = stage;
}

const char *bongo_cat_first_frame_stage_name(void) {
    return current_stage;
}

void bongo_cat_first_frame_watchdog_arm(BongoCatApp *app) {
    started_ns = SDL_GetTicksNS();
    armed = true;
    fell_back = false;
    /*
     * 注意 presented 不能被无条件清零: 装配的最后一步 (首次模型加载) 本身就会画出并
     * 端出第一帧, 那时 bongo_cat_startup_ready() 已经把 presented 置真了 —— 它在
     * arm 之前发生。第一版在这里无条件赋值, 结果正常启动的进程在 8 秒后被误报成
     * "首帧没端出去"(实测 presented_frames=437), 还会把配置改成 OpenGL。
     * 所以只在"还没呈现过"时才按可见性做初始判定。
     */
    if (!presented)
        presented = !app || !app->session.window.visible;
    if (presented) current_stage = "not-visible";
}

void bongo_cat_first_frame_watchdog_presented(void) {
    presented = true;
}

/* GPU/驱动摘要: 两个后端各取各的。GL 上下文不在当前线程时不去猜型号 —— 报告里写
   "见启动日志"比写一个错误的名字强。 */
static void gpu_summary(BongoCatApp *app, char *output, size_t capacity) {
    output[0] = '\0';
    if (app && app->vk) {
        BongoCatVkInfo info = {0};
        if (bongo_cat_vk_info(app->vk, &info))
            snprintf(output, capacity, "%s / 驱动 %s / api %s", info.device,
                info.driver, info.api);
        return;
    }
    if (!output[0])
        snprintf(output, capacity, "OpenGL（型号见启动日志 [runtime] OpenGL 上下文）");
}

static void swapchain_summary(BongoCatApp *app, char *output, size_t capacity) {
    output[0] = '\0';
    BongoCatVkSwapchainInfo info = {0};
    if (app && app->vk && bongo_cat_vk_swapchain_info(app->vk, &info))
        snprintf(output, capacity,
            "swapchain=%dx%d images=%u format=%d present_mode=%d "
            "composite_alpha=0x%x",
            info.width, info.height, info.image_count, info.format,
            info.present_mode, info.composite_alpha);
    else
        snprintf(output, capacity, "swapchain=(未创建)");
}

/* 首帧超时: 打一条能直接引用的 ERROR, 然后回退 OpenGL 并重启一次。 */
static void first_frame_timed_out(BongoCatApp *app, uint64_t elapsed_ns) {
    char gpu[224], swapchain[192];
    gpu_summary(app, gpu, sizeof(gpu));
    swapchain_summary(app, swapchain, sizeof(swapchain));
    float scale = 1.0f;
    int width = 0, height = 0;
    if (app && app->window) {
        scale = SDL_GetWindowDisplayScale(app->window);
        SDL_GetWindowSizeInPixels(app->window, &width, &height);
    }
    const BongoCatRenderer active = bongo_cat_renderer_active();
    SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
        "[watchdog] 首帧在 %.1f 秒内没有端出去: 后端=%s GPU=%s DPI=%.0f%% "
        "窗口=%dx%d 像素 阶段=%s %s presented_frames=%llu",
        (double)elapsed_ns / 1000000000.0, bongo_cat_renderer_name(active), gpu,
        (double)(scale * 100.0f), width, height, current_stage, swapchain,
        (unsigned long long)(app ? app->input_diagnostics.presented_frames : 0));
    /* 只有 Vulkan 有"回退到 OpenGL"这个动作; 已经在 OpenGL 上再报一次也没有出口,
       所以只记录不回退。 */
    if (active != BONGO_CAT_RENDERER_VULKAN || fell_back) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
            "[watchdog] 当前后端已是 %s, 没有可回退的后端%s",
            bongo_cat_renderer_name(active),
            fell_back ? "（本次运行已经回退过一次）" : "");
        armed = false;
        return;
    }
    fell_back = true;
    armed = false;
    app->settings.model.renderer = BONGO_CAT_RENDERER_OPENGL;
    app->dirty = true;
    /* 先落盘: 重启动作在主循环末尾会先 flush, 但这里也显式刷一次 —— 即便重启被拦下
       (无人值守), 用户下次手动启动也已经是 OpenGL 了。 */
    bongo_cat_config_store_flush(app);
    const bool restarting = bongo_cat_app_request_restart(app);
    SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
        "[watchdog] 首帧超时，已回退 OpenGL（写入配置 renderer=opengl），%s",
        restarting ? "正在自动重启" : "自动重启未执行，请手动重启");
}

void bongo_cat_first_frame_watchdog_update(BongoCatApp *app, uint64_t now) {
    if (!armed || presented) return;
    uint64_t elapsed = now - started_ns;
    if (elapsed < BONGO_CAT_FIRST_FRAME_TIMEOUT_NS) return;
    first_frame_timed_out(app, elapsed);
}
