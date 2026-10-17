/*
 * 组件侧的宿主适配层: 把"桥接代码直接调用的宿主符号"改成"经宿主服务表调用"。
 *
 * 为什么需要这一层(而不是改桥接代码): 桥接有上万行, 逐个改调用点既危险又会把 diff 铺得
 * 到处都是。这里反过来 —— **在组件里定义同名的宿主符号**, 让它们转发到服务表。于是:
 *   * 桥接源码一个字都不用改;
 *   * "组件能用宿主的什么"被压缩成这一个文件里的十几行, 一眼看得全;
 *   * 宿主与组件各自编一份同名符号互不干扰(不同的二进制, 不同的符号表)。
 *
 * 严格纪律:
 *   1. 组件**不链接 SDL**: SDL 的状态(窗口/GL 上下文/日志回调)属于宿主进程里的那一份。
 *      组件里若再链一份 SDL, 它看到的上下文是空的 —— 渲染会直接失效。所以 SDL 调用由
 *      component_sdl_shim.h 用宏改道到本文件的转发函数。
 *   2. 宿主私有类型(BongoCatVk* 等)绝不跨界: `bongo_cat_vk_native_info` 收到的 vk 参数
 *      被**忽略**, 改成向宿主服务表要一份只读快照(BongoCatVkNativeInfo 是 POD)。
 *   3. 所有分配都成对: 宿主给的缓冲用 host->release 归还, 组件自己 allocate 的也用它。
 */
#include "component_host.h"

#include "bongo_cat/gl_api.h"
#include "bongo_cat/image.h"
#include "bongo_cat/json.h"
#include "bongo_cat/render.h"
#include "bongo_cat/vk_backend.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const BongoCatRendererModuleHost *services;

void bongo_component_bind(const BongoCatRendererModuleHost *host) {
    services = host;
}

const BongoCatRendererModuleHost *bongo_component_host(void) { return services; }

/* ---- SDL 的替身(见 component_sdl_shim.h 的宏映射) ---- */

void bongo_component_log(int priority, const char *format, ...) {
    if (!services) return;
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format ? format : "", args);
    va_end(args);
    services->log(priority, message);
}

uint64_t bongo_component_ticks_ns(void) {
    return services ? services->ticks_ns() : 0;
}

void *bongo_component_allocate(size_t size) {
    return services ? services->allocate(size) : NULL;
}

void bongo_component_release(void *memory) {
    if (services) services->release(memory);
}

/* 读整个文件: 宿主分配(SDL_LoadFile), 组件用 release 归还 —— 跨 CRT 的缓冲不能混用 free。 */
void *bongo_component_read_file(const char *path, size_t *size) {
    if (size) *size = 0;
    return services ? services->read_file(path, size) : NULL;
}

/* 组件里没有"自己的路径"这回事: 资源根由宿主在 create 时给出, 其余目录走 directory 服务。 */
const char *bongo_component_base_path(void) { return ""; }

const char *bongo_component_error(void) { return "SDL is owned by the host process"; }

void *bongo_component_gl_current_window(void) {
    return services ? services->gl_current_window() : NULL;
}

void *bongo_component_gl_current_context(void) {
    return services ? services->gl_current_context() : NULL;
}

bool bongo_component_gl_make_current(void *window, void *context) {
    return services ? services->gl_make_current(window, context) : false;
}

bool bongo_component_window_size_pixels(void *window, int *width, int *height) {
    return services ? services->window_size_pixels(window, width, height) : false;
}

/* ---- 桥接直接引用的宿主符号(全部转成服务表调用) ---- */

void bongo_cat_error_set(BongoCatError *error, BongoCatResult code,
    const char *format, ...) {
    if (!error) return;
    error->code = code;
    if (!format) {
        error->message[0] = '\0';
        return;
    }
    va_list args;
    va_start(args, format);
    vsnprintf(error->message, sizeof(error->message), format, args);
    va_end(args);
}

BongoCatRenderer bongo_cat_renderer_active(void) {
    /* 服务表给的是后端位; 组件内部的判定只关心"是不是 Vulkan"。 */
    if (services && services->active_backend() == BONGO_CAT_RENDERER_BACKEND_VULKAN)
        return BONGO_CAT_RENDERER_VULKAN;
    return BONGO_CAT_RENDERER_OPENGL;
}

bool bongo_cat_gl_clear_errors(void) {
    return services ? services->gl_clear_errors() : true;
}

BongoCatResult bongo_cat_image_fit(const char *path, int max_width,
    int max_height, BongoCatImage *out, BongoCatError *error) {
    if (!services) return BONGO_CAT_ERROR_PLATFORM;
    return services->image_fit(path, max_width, max_height, out, error);
}

void bongo_cat_image_free(BongoCatImage *image) {
    if (services && image) services->image_free(image);
}

void bongo_cat_image_make_alpha_mask(const BongoCatImage *image,
    BongoCatImageAlphaMask *mask) {
    if (services && image && mask) services->image_make_alpha_mask(image, mask);
}

unsigned int bongo_cat_image_texture_model(const char *path, bool direct_decode,
    int *width, int *height, BongoCatImageAlphaMask *alpha,
    BongoCatImageProgress progress, void *userdata, BongoCatError *error) {
    if (!services) return 0;
    return services->image_texture_model(path, direct_decode, width, height,
        alpha, progress, userdata, error);
}

/*
 * 模型图集工作尺寸上限: 问宿主 (唯一判定处, 见 ABI 的 model_texture_limit)。
 * 宿主没提供这一项时退回编译期默认值 —— 组件与宿主版本不一致时仍然能画出模型, 只是
 * "纹理质量"这一档设置不生效。
 */
int bongo_cat_image_model_texture_limit(void) {
    if (services && services->model_texture_limit)
        return services->model_texture_limit();
    return BONGO_CAT_LIVE2D_TEXTURE_LIMIT;
}

yyjson_doc *bongo_cat_model_json_parse(const char *data, size_t size,
    bool *normalized) {
    if (!services) return NULL;
    return services->model_json_parse(data, size, normalized);
}

/*
 * 解码/上传诊断探针(bongo_cat_image_log_upload_probe)。
 *
 * 宿主那份在 media/image.c 里按 BONGO_CAT_TEST_DECODE_PROBE 工作, 读的是**宿主**的计数器;
 * 组件里再算一份只会得到两个不一致的数字。所以这里只把尺寸写进宿主日志, 保留"看得见"这一点,
 * 而不假装自己有一份等价统计(那比没有更糟)。
 */
void bongo_cat_image_log_upload_probe(const BongoCatImage *image,
    const char *where) {
    if (!services || !image) return;
    char message[160];
    snprintf(message, sizeof(message), "%s: %dx%d upload probe (component side)",
        where ? where : "image", image->width, image->height);
    services->log(3 /* SDL_LOG_PRIORITY_DEBUG */, message);
}

bool bongo_cat_vk_native_info(const BongoCatVk *vk, BongoCatVkNativeInfo *info) {
    /* vk 参数**故意忽略**: 那是宿主私有的结构, 组件不该也不能解释它。
       要设备/队列就向服务表要一份只读快照 —— 这正是 2d 要确立的边界。 */
    (void)vk;
    if (!services) return false;
    return services->vulkan_native_info(info);
}

/* Cubism 渲染后端的运行期开关: 由宿主配置层回答, 组件不自己决定。 */
bool bongo_cat_cubism_renderer_uses_vulkan(void) {
    return services ? services->cubism_renderer_uses_vulkan() : false;
}

void bongo_cat_cubism_renderer_use_vulkan(bool enabled) {
    if (services) services->cubism_renderer_set_vulkan(enabled);
}

/* 生成的 shim(cmake/GenCoreShim.ps1)的解析入口: 组件侧走服务表 core_symbol,
   于是同一份 shim 既能编进宿主也能编进组件。 */
void *bongo_cat_cubism_core_resolve(const char *name) {
    return services ? services->core_symbol(name) : NULL;
}
