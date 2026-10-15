/*
 * Vulkan 专有入口的分发(阶段 2d)。
 *
 * 与 live2d_dispatch.c 同一套规则(组件在且有该回调就交给它, 否则回落诊断实现), 只是多一层
 * **借用语义**: `target_info` 交出的 image/view/sampler 由**组件**创建并拥有, 宿主只在这一次
 * 合成里借用 —— 组件重建离屏目标时旧句柄立即失效, 宿主不得缓存、不得销毁(见 live2d_vulkan.h
 * 与 renderer_module_abi.h 里的说明)。
 *
 * 宿主把设备交给组件的方式也在这里: remember() 收到的是宿主的 `BongoCatVk*`(宿主侧内部
 * 类型), 本文件立刻把它转成**只读 POD 快照**再交给组件 —— 于是宿主的私有结构体从不跨界,
 * 而 window.c 的调用点一个字都不用改。
 */
#include "live2d_dispatch.h"

#include "bongo_cat/live2d_vulkan.h"
#include "bongo_cat/log.h"
#include "bongo_cat/renderer_module.h"
#include "bongo_cat/renderer_module_abi.h"
#include "bongo_cat/vk_backend.h"

#include <SDL3/SDL.h>

#define STUB_VK(name) stub_live2d_vulkan_##name

extern bool STUB_VK(remember)(const BongoCatVkNativeInfo *info, int width, int height);
extern bool STUB_VK(attach)(const BongoCatVkNativeInfo *info, int width, int height);
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
extern uint64_t STUB_VK(target_generation)(void);
extern void *STUB_VK(create_texture)(const void *pixels, int width, int height);
extern void *STUB_VK(create_texture_attached)(const void *pixels, int width,
    int height);
extern void STUB_VK(destroy_texture)(void *texture);
extern bool STUB_VK(bind_texture)(void *renderer, void *texture);

static const BongoCatRendererModuleOps *ops(void) {
    if (!bongo_cat_live2d_native_active()) return NULL;
    const BongoCatRendererModule *module =
        bongo_cat_renderer_module_active_descriptor();
    return module ? &module->ops : NULL;
}

bool bongo_cat_live2d_vulkan_remember_backend(struct BongoCatVk *vk, int width,
    int height) {
    if (!vk || width <= 0 || height <= 0) return false;
    /* 私有结构体只在这一行出现: 立刻抽成 POD 快照, 之后一路只传快照。 */
    BongoCatVkNativeInfo info = {0};
    if (!bongo_cat_vk_native_info(vk, &info)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "[live2d/vk] remember: 后端句柄不可用(vk=%p)", (void *)vk);
        return false;
    }
    /* 组件是懒装载的, 而窗口创建早于第一个模型: 这里必须先把它装上, 否则宿主会误判成
       "这次没有 VK 渲染器"而回退 GL。 */
    bongo_cat_live2d_ensure_component();
    const BongoCatRendererModuleOps *active = ops();
    if (active && active->vulkan_remember)
        return active->vulkan_remember(&info, width, height);
    /* 没有组件就没有 Vulkan 的 Live2D 渲染器: 明确返回 false, 让宿主沿用既有的
       "Vulkan 渲染器未登记 -> 回退 OpenGL" 路径(那条路径自己会记警告)。 */
    return false;
}

bool bongo_cat_live2d_vulkan_attach(const BongoCatVkNativeInfo *info, int width,
    int height) {
    const BongoCatRendererModuleOps *active = ops();
    if (active && active->vulkan_attach)
        return active->vulkan_attach(info, width, height);
    return STUB_VK(attach)(info, width, height);
}

void bongo_cat_live2d_vulkan_detach(void) {
    const BongoCatRendererModuleOps *active = ops();
    if (active && active->vulkan_detach) { active->vulkan_detach(); return; }
    STUB_VK(detach)();
}

bool bongo_cat_live2d_vulkan_ready(void) {
    const BongoCatRendererModuleOps *active = ops();
    if (active && active->vulkan_ready) return active->vulkan_ready();
    return STUB_VK(ready)();
}

bool bongo_cat_live2d_vulkan_shaders_available(const char *assets_root) {
    const BongoCatRendererModuleOps *active = ops();
    if (active && active->vulkan_shaders_available)
        return active->vulkan_shaders_available(assets_root);
    return STUB_VK(shaders_available)(assets_root);
}

void bongo_cat_live2d_vulkan_set_shader_root(const char *assets_root) {
    const BongoCatRendererModuleOps *active = ops();
    if (active && active->vulkan_set_shader_root) {
        active->vulkan_set_shader_root(assets_root);
        return;
    }
    STUB_VK(set_shader_root)(assets_root);
}

void bongo_cat_live2d_vulkan_set_supersample(int mode) {
    const BongoCatRendererModuleOps *active = ops();
    if (active && active->vulkan_set_supersample) {
        active->vulkan_set_supersample(mode);
        return;
    }
    STUB_VK(set_supersample)(mode);
}

bool bongo_cat_live2d_vulkan_begin_frame(void) {
    const BongoCatRendererModuleOps *active = ops();
    if (active && active->vulkan_begin_frame) return active->vulkan_begin_frame();
    return STUB_VK(begin_frame)();
}

bool bongo_cat_live2d_vulkan_end_frame(void) {
    const BongoCatRendererModuleOps *active = ops();
    if (active && active->vulkan_end_frame) return active->vulkan_end_frame();
    return STUB_VK(end_frame)();
}

bool bongo_cat_live2d_vulkan_target_info(void **image, void **view, void **sampler,
    int *width, int *height) {
    const BongoCatRendererModuleOps *active = ops();
    if (active && active->vulkan_target_info)
        return active->vulkan_target_info(image, view, sampler, width, height);
    return STUB_VK(target_info)(image, view, sampler, width, height);
}

uint64_t bongo_cat_live2d_vulkan_target_generation(void) {
    const BongoCatRendererModuleOps *active = ops();
    if (active && active->vulkan_target_generation)
        return active->vulkan_target_generation();
    return STUB_VK(target_generation)();
}

void bongo_cat_live2d_vulkan_release_target(void) {
    const BongoCatRendererModuleOps *active = ops();
    if (active && active->vulkan_release_target) {
        active->vulkan_release_target();
        return;
    }
    STUB_VK(release_target)();
}

void *bongo_cat_live2d_vulkan_create_texture(const void *pixels, int width,
    int height) {
    const BongoCatRendererModuleOps *active = ops();
    if (active && active->vulkan_create_texture)
        return active->vulkan_create_texture(pixels, width, height);
    return STUB_VK(create_texture)(pixels, width, height);
}

void *bongo_cat_live2d_vulkan_create_texture_attached(const void *pixels,
    int width, int height) {
    const BongoCatRendererModuleOps *active = ops();
    if (active && active->vulkan_create_texture_attached)
        return active->vulkan_create_texture_attached(pixels, width, height);
    return STUB_VK(create_texture_attached)(pixels, width, height);
}

void bongo_cat_live2d_vulkan_destroy_texture(void *texture) {
    const BongoCatRendererModuleOps *active = ops();
    if (active && active->vulkan_destroy_texture) {
        active->vulkan_destroy_texture(texture);
        return;
    }
    STUB_VK(destroy_texture)(texture);
}

bool bongo_cat_live2d_vulkan_bind_texture(void *renderer, void *texture) {
    const BongoCatRendererModuleOps *active = ops();
    if (active && active->vulkan_bind_texture)
        return active->vulkan_bind_texture(renderer, texture);
    return STUB_VK(bind_texture)(renderer, texture);
}
