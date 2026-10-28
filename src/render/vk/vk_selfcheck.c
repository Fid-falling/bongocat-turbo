/*
 * 自检里的 Vulkan 首帧探测 (设置 → 应用 → [运行自检])。
 *
 * 为什么必须单独一份: 用户报告的故障是"日志停在 create_renderer 之后, 进程闪一下
 * 就消失", 而首帧呈现附近几乎没有日志。光看主窗口的日志分不清"设备/交换链根本建不
 * 出来"与"建出来了但端不出去"。这个探测在一个**独立的隐藏窗口**上把整条链走一遍:
 *   设备 (设备名/驱动/apiVersion) -> 表面 -> 交换链 (present mode/compositeAlpha)
 *   -> 录制 1 帧 -> 提交 -> present
 * 过程完全不碰宠物窗口: 不登记 bongo_cat_vk_current(), 不销毁别人的设备。
 *
 * 复用主设备还是独立实例: 主窗口已经有 Vulkan 上下文时优先 create_shared (一个进程
 * 只保留一套 VkDevice 是刻意设计, 见 vk_backend.h); 主窗口跑的是 OpenGL (典型场景:
 * 上次 Vulkan 首帧失败自动回退了) 时才建一套独立的实例/设备 —— 那种情况下"这台机器
 * 现在能不能跑 Vulkan"本来就没有现成设备可问。
 *
 * 失败一律写 report->error 的完整句子: 自检面板要把它原样展示给用户, 不能只给一个
 * false。
 */
#include "vk_internal.h"
#include "bongo_cat/log.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#ifdef BONGO_CAT_HAS_VULKAN

/* 探测窗口只用来承载一次 present, 尺寸取一个够小又能被驱动接受的值 (0 尺寸表面
   在部分驱动上直接拒绝建交换链)。 */
#define BONGO_CAT_VK_SELFCHECK_WIDTH 64
#define BONGO_CAT_VK_SELFCHECK_HEIGHT 64

static void set_failure(BongoCatVkSelfCheck *report, const char *text) {
    if (!report) return;
    snprintf(report->error, sizeof(report->error), "%s",
        text && text[0] ? text : "Vulkan 首帧探测失败");
}

/* prefix + "：" + detail, 用于"哪个阶段 + 驱动给的原因"这一类消息 */
static void set_failure_detail(BongoCatVkSelfCheck *report, const char *prefix,
    const char *detail) {
    if (!report) return;
    snprintf(report->error, sizeof(report->error), "%s: %s",
        prefix ? prefix : "Vulkan 首帧探测失败",
        detail && detail[0] ? detail : "未知原因");
}

/* 探测窗口用 SDL_WINDOW_TRANSPARENT: 宠物窗口本身就是透明窗口, 而用户日志里出现过
   "compositeAlpha 创建失败退回不透明合成" —— 用不透明窗口探测会把这条差异藏起来。 */
static SDL_Window *create_probe_window(void) {
    return SDL_CreateWindow("BongoCat Vulkan Self-Check",
        BONGO_CAT_VK_SELFCHECK_WIDTH, BONGO_CAT_VK_SELFCHECK_HEIGHT,
        SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN | SDL_WINDOW_TRANSPARENT |
        SDL_WINDOW_NOT_FOCUSABLE);
}

static void destroy_probe_window(SDL_Window *window) {
    if (window) SDL_DestroyWindow(window);
}

bool bongo_cat_vk_selfcheck(BongoCatVkSelfCheck *report) {
    BongoCatVkSelfCheck local;
    if (!report) report = &local;
    memset(report, 0, sizeof(*report));
    report->format = -1;
    report->present_result = -1;
    report->compiled = bongo_cat_vk_compiled();
    const uint64_t started_ns = SDL_GetTicksNS();
    /* 测试钩子 (与 bongo_cat_vk_probe 同一个): 让"这台机器上 Vulkan 不可用"可复现,
       自检面板的失败态与报告文案才测得到。 */
    if (SDL_getenv("BONGO_CAT_TEST_VULKAN_UNAVAILABLE")) {
        set_failure(report, "测试钩子 BONGO_CAT_TEST_VULKAN_UNAVAILABLE");
        return false;
    }
    if (!report->compiled) {
        set_failure(report,
            "此构建未启用 Vulkan (需要 -DBONGO_CAT_ENABLE_VULKAN=ON)");
        return false;
    }
    SDL_Window *window = create_probe_window();
    if (!window) {
        set_failure_detail(report, "无法创建探测用的隐藏窗口", SDL_GetError());
        return false;
    }
    /*
     * 优先复用主窗口的设备: 它已经在跑, 说明驱动/设备这条线是通的; 我们只需要验证
     * "这台机器上还能再建一条交换链并端出一帧"。没有主上下文 (当前是 OpenGL) 时
     * 才建独立实例。owner 只借用, 销毁时不会动它。
     */
    BongoCatVk *owner = bongo_cat_vk_current();
    BongoCatError error = {0};
    BongoCatVk *vk = bongo_cat_vk_create_isolated(window, owner, &error);
    if (!vk) {
        set_failure_detail(report, "Vulkan 上下文创建失败",
            error.message[0] ? error.message : "未知原因");
        destroy_probe_window(window);
        report->elapsed_ms = (unsigned)((SDL_GetTicksNS() - started_ns) / 1000000ull);
        return false;
    }
    report->shared = owner != NULL;
    snprintf(report->device, sizeof(report->device), "%s", vk->info.device);
    snprintf(report->driver, sizeof(report->driver), "%s", vk->info.driver);
    snprintf(report->api, sizeof(report->api), "%s", vk->info.api);
    report->present_modes = vk->present_modes_supported;
    report->composite_supported = (unsigned)vk->composite_supported;
    report->composite_alpha = (unsigned)vk->composite_alpha;
    report->format = (int)vk->format;
    report->width = (int)vk->extent.width;
    report->height = (int)vk->extent.height;
    report->swapchain_created = vk->extent_valid;
    bool began = false;
    if (report->swapchain_created) {
        /* clear 成全透明: 透明窗口的呈现路径与宠物窗口一致 */
        bongo_cat_vk_set_clear_color(vk, 0.0f, 0.0f, 0.0f, 0.0f);
        began = bongo_cat_vk_begin_frame(vk, (int)vk->extent.width,
            (int)vk->extent.height);
    }
    if (began) report->presented = bongo_cat_vk_end_frame(vk);
    report->present_result = vk->last_present_result;
    if (!report->swapchain_created)
        set_failure_detail(report, "交换链未能创建",
            "驱动拒绝了 present mode / compositeAlpha 组合");
    else if (!began)
        set_failure_detail(report, "首帧开始失败",
            bongo_cat_vk_result_name((VkResult)vk->last_acquire_result));
    else if (!report->presented)
        set_failure_detail(report, "首帧呈现失败",
            bongo_cat_vk_result_name((VkResult)vk->last_present_result));
    else
        report->error[0] = '\0';
    /*
     * 探测结果进日志: 用户回传日志时, 自检报告本身也要能在日志里对着看。只用
     * LIFECYCLE 类别, 因为 Release 构建下 INFO 只放行那三个自定义类别。
     */
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[selfcheck] Vulkan 首帧探测: ok=%d shared=%d device=%s driver=%s api=%s "
        "present_modes=0x%x composite_alpha=0x%x supported=0x%x swapchain=%dx%d "
        "present_result=%d error=%s",
        report->presented ? 1 : 0, report->shared ? 1 : 0, report->device,
        report->driver, report->api, report->present_modes,
        report->composite_alpha, report->composite_supported, report->width,
        report->height, report->present_result,
        report->error[0] ? report->error : "none");
    bongo_cat_vk_destroy(vk);
    destroy_probe_window(window);
    report->elapsed_ms = (unsigned)((SDL_GetTicksNS() - started_ns) / 1000000ull);
    return report->presented;
}

#endif /* BONGO_CAT_HAS_VULKAN */
