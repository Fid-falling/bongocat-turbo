/*
 * Vulkan 专有入口的分发(阶段 2d 的过渡形态)。
 *
 * `bongo_cat_live2d_vulkan_*` 这一组函数是给宿主的 VK 合成路径用的(vk_scene.c 拿离屏目标、
 * 绑纹理、走一帧的 begin/end)。**阶段 2c 只做 GL 验收**, 而组件的 VK 契约要等 2d 才定型:
 * 现在把这些入口全部交给诊断实现(即"没有 VK 离屏目标"), 并在"组件已经在跑却还有人调 VK 入口"
 * 时**明确记一条警告** —— 沉默地返回 false 会让 VK 用户看到"模型不显示"却查不出原因,
 * 而那正是这一层最该避免的失败形态。
 *
 * 2d 要做的: 把这组入口改成走组件(设备/队列/命令池能力面 + 组件自持离屏目标), 那时这个
 * 文件里的"警告 + 回落"就会变成真正的转发。
 */
#include "live2d_dispatch.h"

#include "bongo_cat/live2d_vulkan.h"
#include "bongo_cat/log.h"

#include <SDL3/SDL.h>

#define STUB_VK(name) stub_live2d_vulkan_##name

extern bool STUB_VK(attach)(struct BongoCatVk *vk, int width, int height);
extern void STUB_VK(detach)(void);
extern bool STUB_VK(ready)(void);
extern bool STUB_VK(shaders_available)(const char *assets_root);
extern void STUB_VK(set_shader_root)(const char *assets_root);
extern void STUB_VK(set_supersample)(int mode);
extern bool STUB_VK(begin_frame)(void);
extern bool STUB_VK(end_frame)(void);
extern bool STUB_VK(target_info)(void **image, void **view, void **sampler,
    int *width, int *height);
extern void STUB_VK(release_target)(void);
extern void *STUB_VK(create_texture)(struct BongoCatVk *vk, const void *pixels,
    int width, int height);
extern void *STUB_VK(create_texture_attached)(const void *pixels, int width,
    int height);
extern void STUB_VK(destroy_texture)(void *texture);
extern bool STUB_VK(bind_texture)(void *renderer, void *texture);

/* 只警告一次: VK 路径每帧都会问 ready(), 每帧记一条会把日志冲掉。 */
static void warn_once(const char *entry) {
    static bool warned;
    if (warned || !bongo_cat_live2d_native_active()) return;
    warned = true;
    SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
        "[live2d] 渲染组件已接管 GL 渲染, 但 Vulkan 专有入口(%s)仍在走诊断实现 —— "
        "VK 后端的组件化是阶段 2d, 当前 VK 下 Live2D 不会出图", entry);
}

/*
 * 宿主窗口创建后会把 VK 设备信息交给 Live2D 桥("记住它")。
 *
 * 实现搬到组件之后, 宿主这一侧只需要**记录**这件事由谁负责: 真正需要设备信息的是组件的
 * VK 路径, 而它拿到的方式是宿主服务表的 vulkan_native_info(2d 的契约)。所以这里返回 false
 * (= "宿主没有替组件记下什么"), 让 window.c 记一条日志而不是静默成功 —— 这也是 2d 要在
 * 这里补上的那一步。
 */
bool bongo_cat_live2d_vulkan_remember(struct BongoCatVk *vk, int width, int height) {
    (void)vk;
    (void)width;
    (void)height;
    warn_once("remember");
    return false;
}

bool bongo_cat_live2d_vulkan_attach(struct BongoCatVk *vk, int width, int height) {
    warn_once("attach");
    return STUB_VK(attach)(vk, width, height);
}

void bongo_cat_live2d_vulkan_detach(void) {
    warn_once("detach");
    STUB_VK(detach)();
}

bool bongo_cat_live2d_vulkan_ready(void) {
    warn_once("ready");
    return STUB_VK(ready)();
}

bool bongo_cat_live2d_vulkan_shaders_available(const char *assets_root) {
    warn_once("shaders_available");
    return STUB_VK(shaders_available)(assets_root);
}

void bongo_cat_live2d_vulkan_set_shader_root(const char *assets_root) {
    warn_once("set_shader_root");
    STUB_VK(set_shader_root)(assets_root);
}

void bongo_cat_live2d_vulkan_set_supersample(int mode) {
    warn_once("set_supersample");
    STUB_VK(set_supersample)(mode);
}

bool bongo_cat_live2d_vulkan_begin_frame(void) {
    warn_once("begin_frame");
    return STUB_VK(begin_frame)();
}

bool bongo_cat_live2d_vulkan_end_frame(void) {
    warn_once("end_frame");
    return STUB_VK(end_frame)();
}

bool bongo_cat_live2d_vulkan_target_info(void **image, void **view, void **sampler,
    int *width, int *height) {
    warn_once("target_info");
    return STUB_VK(target_info)(image, view, sampler, width, height);
}

void bongo_cat_live2d_vulkan_release_target(void) {
    warn_once("release_target");
    STUB_VK(release_target)();
}

void *bongo_cat_live2d_vulkan_create_texture(struct BongoCatVk *vk,
    const void *pixels, int width, int height) {
    warn_once("create_texture");
    return STUB_VK(create_texture)(vk, pixels, width, height);
}

void *bongo_cat_live2d_vulkan_create_texture_attached(const void *pixels,
    int width, int height) {
    warn_once("create_texture_attached");
    return STUB_VK(create_texture_attached)(pixels, width, height);
}

void bongo_cat_live2d_vulkan_destroy_texture(void *texture) {
    warn_once("destroy_texture");
    STUB_VK(destroy_texture)(texture);
}

bool bongo_cat_live2d_vulkan_bind_texture(void *renderer, void *texture) {
    warn_once("bind_texture");
    return STUB_VK(bind_texture)(renderer, texture);
}
