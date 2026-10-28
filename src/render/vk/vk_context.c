/*
 * Vulkan 上下文的生命周期与查询, 从 vk_core.c 拆出来只为守住单文件 500 行
 * (cmake/CheckLines.cmake): vk_core.c 管 Instance/Device 的创建与销毁, 这里管
 * "一个窗口的上下文" (表面 + 交换链 + 管线 + 帧同步的装配顺序), 以及对外查询。
 *
 * 装配顺序是本文件的核心不变量, 不能调换:
 *   instance (共享时借用) -> surface -> device (共享时借用)
 *   -> frame_sync (命令池) -> texture_resources (描述符池/采样器)
 *   -> swapchain (渲染通道 + 管线要依赖它) -> draw_resources
 * 中间任何一步失败都走 bongo_cat_vk_destroy, 它按相反顺序拆。
 */
#include "vk_internal.h"
#include "bongo_cat/log.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef BONGO_CAT_HAS_VULKAN

/*
 * 探测失败后的重试窗口。见 bongo_cat_vk_probe: 失败不能被永久缓存 (瞬时状态会被误判成
 * "这台机器没有 Vulkan"), 但设置界面每帧都会问一次可用性, 所以也不能每次真去探测。
 * 3 秒足够让"驱动刚起来/设备刚被释放"这类情况自愈, 又不会让每帧都付出一次实例创建的
 * 代价。
 */
#define BONGO_CAT_VK_PROBE_RETRY_NS 3000000000ull

bool bongo_cat_vk_compiled(void) { return true; }

static BongoCatVk *current_context;

BongoCatVk *bongo_cat_vk_current(void) { return current_context; }

bool bongo_cat_vk_probe(BongoCatError *error) {
    /*
     * 测试钩子: 让"这台机器上 Vulkan 不可用"变成可复现的。拒绝路径 (日志 + 用户可见
     * 提示 + 绝不写配置) 只在探测失败时才走得到, 而开发机上几乎永远探测成功 ——
     * 与 BONGO_CAT_TEST_VULKAN_FALLBACK / BONGO_CAT_TEST_SHARED_VK_FAILURE 同一套路。
     * 默认不存在这个变量, 不影响正常运行。
     */
    if (SDL_getenv("BONGO_CAT_TEST_VULKAN_UNAVAILABLE")) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "test hook BONGO_CAT_TEST_VULKAN_UNAVAILABLE");
        return false;
    }
    /*
     * 缓存策略: 成功永久缓存, **失败只缓存一小会儿**。
     * 为什么不能把失败也永久缓存: 第一次探测可能撞上"驱动还在初始化/设备被别的进程
     * 独占/远程会话刚切换"这类瞬时状态, 一旦永久记成"不可用", 设置界面里那一段就会
     * 一直画成禁用、点了没反应, 直到用户重启程序 —— 用户报告的现象就是这个。
     * 也不能每次调用都重新探测: 设置界面每帧都会问一次可用性, 每次都去建一个
     * VkInstance 太贵 (失败路径上尤其贵)。所以给失败设一个很短的重试窗口。
     */
    static int cached;            /* 0 = 还没成功过, 1 = 这台机器确实有 Vulkan */
    static uint64_t failed_ns;    /* 上次失败的时刻, 只在失败窗口内有意义 */
    uint64_t now = SDL_GetTicksNS();
    if (cached == 1) return true;
    if (failed_ns && now - failed_ns < BONGO_CAT_VK_PROBE_RETRY_NS) return false;
    BongoCatVk probe = {0};
    BongoCatError local = {0};
    BongoCatError *failure = error ? error : &local;
    bool usable = bongo_cat_vk_create_instance(&probe, failure) &&
        bongo_cat_vk_create_device(&probe, failure);
    bongo_cat_vk_destroy_device(&probe);
    if (usable) {
        cached = 1;
        failed_ns = 0;
        return true;
    }
    failed_ns = now ? now : 1;
    return false;
}

static BongoCatVk *create_context(SDL_Window *window, const BongoCatVk *owner,
    bool register_current, BongoCatError *error) {
    if (!window) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_ARGUMENT, "Vulkan 后端缺少窗口");
        return NULL;
    }
    if (owner && owner->device == VK_NULL_HANDLE) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_ARGUMENT,
            "Vulkan 共享上下文的主设备无效");
        return NULL;
    }
    BongoCatVk *vk = calloc(1, sizeof(*vk));
    if (!vk) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY, "Vulkan 后端状态分配失败");
        return NULL;
    }
    vk->window = window;
    /* 默认跟着 OpenGL 路径的老行为 (垂直同步开); 运行时再按最大帧率设置更新 */
    vk->vsync = true;
    if (owner) {
        /* 复用主窗口的 instance/device/queue: 只借句柄, 不接管所有权 */
        vk->instance = owner->instance;
        vk->physical = owner->physical;
        vk->device = owner->device;
        vk->queue = owner->queue;
        vk->queue_family = owner->queue_family;
        vk->owns_device = false;
        /*
         * 校验层是 instance 级的开关: 共享上下文自己没有 messenger (消息由主上下文
         * 打), 但"这台机器的 instance 到底开没开校验层"必须如实反映在日志里 ——
         * 每个上下文各报一个 validation 值会让同一次运行出现两种说法。
         */
        vk->validation = owner->validation;
        /*
         * info 的 device/driver/api 三个字段来自 vkGetPhysicalDeviceProperties, 而
         * 共享上下文不会去重新枚举设备 —— 不在这里显式继承, 那条 "设备就绪
         * device= driver= api=" 日志就会是三个空字段。
         *
         * 实测代价: 用户反馈里的设置窗口/轮盘两行 "设备就绪" 全是空的, 于是"到底
         * 跑在哪块 GPU、哪个驱动上"在第一手日志里就查不到了 —— 而这恰好是图形故障
         * 最需要的两个信息。宽度/高度仍然由各自的交换链填写 (两个窗口尺寸不同)。
         */
        vk->info = owner->info;
        vk->info.width = 0;
        vk->info.height = 0;
    } else {
        vk->owns_device = true;
    }
    if (!owner && !bongo_cat_vk_create_instance(vk, error)) {
        bongo_cat_vk_destroy(vk);
        return NULL;
    }
    int width = 0, height = 0;
    SDL_GetWindowSizeInPixels(window, &width, &height);
    if (width <= 0 || height <= 0) { width = 1; height = 1; }
    if (!SDL_Vulkan_CreateSurface(window, vk->instance, NULL, &vk->surface)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM, "Vulkan 表面创建失败: %s",
            SDL_GetError());
        bongo_cat_vk_destroy(vk);
        return NULL;
    }
    if (!owner && !bongo_cat_vk_create_device(vk, error)) {
        bongo_cat_vk_destroy(vk);
        return NULL;
    }
    /* 顺序有依赖: 命令池 (帧同步) 要早于纹理上传, 管线要晚于渲染通道。 */
    if (!bongo_cat_vk_create_frame_sync(vk, error) ||
        !bongo_cat_vk_create_texture_resources(vk, error) ||
        !bongo_cat_vk_create_swapchain(vk, width, height, error) ||
        !bongo_cat_vk_create_draw_resources(vk, error)) {
        bongo_cat_vk_destroy(vk);
        return NULL;
    }
    vk->info.width = (int)vk->extent.width;
    vk->info.height = (int)vk->extent.height;
    /* 单例只登记主上下文: 叠加层绘制走 bongo_cat_vk_current(), 不能拿到设置窗口的。
       自检的隐藏窗口上下文更不能顶掉主上下文 —— 否则探测一跑完, 宠物窗口的叠加层
       就再也拿不到自己的设备了。 */
    if (!owner && register_current) current_context = vk;
    /*
     * "设备就绪"是图形故障的第一手证据, 所以字段一个都不能空: device/driver/api 由
     * create_device 填写 (共享上下文从 owner 继承), swapchain/format/present_mode
     * 由刚刚建好的交换链填写。window 指针用来区分同一份日志里的多个上下文
     * (主窗口/设置窗口/轮盘各一条)。
     */
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[vulkan] 设备就绪 device=%s driver=%s api=%s swapchain=%ux%u images=%u "
        "format=%d present_mode=%d composite_alpha=0x%x validation=%d shared=%d "
        "window=%p",
        vk->info.device, vk->info.driver, vk->info.api,
        (unsigned)vk->extent.width, (unsigned)vk->extent.height,
        vk->image_count, (int)vk->format, (int)vk->present_mode,
        (unsigned)vk->composite_alpha, vk->validation ? 1 : 0,
        owner ? 1 : 0, (void *)window);
    /* 共享上下文的这三个字段是继承来的: 继承链断了就在这里自曝, 而不是打一行空字段。 */
    if (owner && vk->info.device[0] == '\0')
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "[vulkan] 共享上下文没有拿到设备信息 (device/driver/api 为空), "
            "请把这一行连同上一条 设备就绪 一起回传");
    return vk;
}

BongoCatVk *bongo_cat_vk_create(SDL_Window *window, BongoCatError *error) {
    return create_context(window, NULL, true, error);
}

/*
 * 自检专用: 与 bongo_cat_vk_create 完全同一条装配路径, 但**不登记为当前上下文**。
 * 探测的隐藏窗口是一份临时上下文, 探测结束就会销毁; 一旦它登记成 current,
 * 宠物窗口的 bongo_cat_vk_current() 会在探测之后变成悬空/空指针。
 */
BongoCatVk *bongo_cat_vk_create_isolated(SDL_Window *window,
    const BongoCatVk *owner, BongoCatError *error) {
    return create_context(window, owner, false, error);
}

BongoCatVk *bongo_cat_vk_create_shared(SDL_Window *window, const BongoCatVk *owner,
    BongoCatError *error) {
    /*
     * 测试钩子, 与 BONGO_CAT_TEST_VULKAN_FALLBACK (window.c) 同一套路: 让"第二个窗口
     * 的 Vulkan 通路"必定失败, 用来验证设置窗口与轮盘的 OpenGL 回退真的会走 ——
     * 那条路径只在用户的机器上出问题时才自然触发, 本机复现不了就只能靠钩子。
     * 默认不存在这个变量, 不影响正常运行。
     */
    if (owner && SDL_getenv("BONGO_CAT_TEST_SHARED_VK_FAILURE")) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "test hook BONGO_CAT_TEST_SHARED_VK_FAILURE");
        return NULL;
    }
    if (!owner) return bongo_cat_vk_create(window, error);
    return create_context(window, owner, true, error);
}

void bongo_cat_vk_destroy(BongoCatVk *vk) {
    if (!vk) return;
    if (current_context == vk) current_context = NULL;
    if (vk->device != VK_NULL_HANDLE) {
        if (!vk->owns_device) vkDeviceWaitIdle(vk->device);
        /* 管线先于渲染通道销毁, 纹理后于使用它们的帧同步销毁 */
        bongo_cat_vk_destroy_draw_resources(vk);
        bongo_cat_vk_destroy_swapchain(vk);
        bongo_cat_vk_destroy_texture_resources(vk);
        bongo_cat_vk_destroy_frame_sync(vk);
    }
    if (vk->owns_device) {
        bongo_cat_vk_destroy_device(vk);
    } else if (vk->surface != VK_NULL_HANDLE && vk->instance != VK_NULL_HANDLE) {
        /* 表面是自己建的, 用借来的 instance 销毁; device/queue 归主上下文 */
        vkDestroySurfaceKHR(vk->instance, vk->surface, NULL);
        vk->surface = VK_NULL_HANDLE;
    }
    free(vk);
}

bool bongo_cat_vk_info(const BongoCatVk *vk, BongoCatVkInfo *info) {
    if (!vk || !info) return false;
    *info = vk->info;
    return true;
}

bool bongo_cat_vk_swapchain_info(const BongoCatVk *vk,
    BongoCatVkSwapchainInfo *info) {
    if (!vk || !info || !vk->extent_valid) return false;
    info->width = (int)vk->extent.width;
    info->height = (int)vk->extent.height;
    info->image_count = vk->image_count;
    info->format = (int)vk->format;
    info->present_mode = (int)vk->present_mode;
    info->composite_alpha = (unsigned)vk->composite_alpha;
    info->composite_supported = (unsigned)vk->composite_supported;
    info->present_modes = vk->present_modes_supported;
    return true;
}

uint32_t bongo_cat_vk_consecutive_present_failures(const BongoCatVk *vk) {
    return vk ? vk->consecutive_present_failures : 0;
}

uint64_t bongo_cat_vk_present_failures(const BongoCatVk *vk) {
    return vk ? vk->present_failures_total : 0;
}

uint64_t bongo_cat_vk_presented_frames(const BongoCatVk *vk) {
    return vk ? vk->presented_total : 0;
}

void bongo_cat_vk_set_clear_color(BongoCatVk *vk, float red, float green,
    float blue, float alpha) {
    if (!vk) return;
    vk->clear.float32[0] = red;
    vk->clear.float32[1] = green;
    vk->clear.float32[2] = blue;
    vk->clear.float32[3] = alpha;
}

#endif /* BONGO_CAT_HAS_VULKAN */
