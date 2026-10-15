/*
 * 宿主服务表的实现(组件能用到的**全部**外部能力)。
 *
 * 为什么要单独成文件: 表是组件的唯一入口, 每一项都是一个"跨界契约"(谁分配谁释放、
 * 谁保证上下文 current、返回值能不能为空)。集中在一处才能一眼看全这些约定, 也才能在
 * 加服务时顺手检查"这一项会不会把宿主私有类型带过界"。
 *
 * 表的地址存进组件, 生命周期必须覆盖组件整个存活期, 所以是**静态**对象, 不是局部变量。
 * 目录服务要用到的 app 在启动期绑定、退出时解绑(悬空指针比"暂时不可用"危险得多)。
 */
#include "bongo_cat/gl_api.h"
#include "bongo_cat/image.h"
#include "bongo_cat/live2d_core.h"
#include "bongo_cat/log.h"
#include "bongo_cat/render.h"
#include "bongo_cat/renderer_module.h"
#include "bongo_cat/renderer_module_abi.h"
#include "bongo_cat/vk_backend.h"

#include <SDL3/SDL.h>

/*
 * Cubism 渲染器走不走 Vulkan: 这份状态现在由**宿主**持有, 组件通过服务表问它 / 回报它。
 *
 * 阶段 2c 之前它在 src/live2d/cubism_renderer_factory.cpp(实现那一侧); 实现搬进组件之后,
 * 宿主再留一份"实现的开关"就没有意义了 —— 这里改成宿主自己的状态: 宿主决定给组件哪个后端,
 * 组件回报它实际用了哪个(这样 VK 合成路径与日志才有单一事实来源)。
 */
static bool host_component_uses_vulkan;

bool bongo_cat_renderer_select_uses_vulkan(void) {
    return host_component_uses_vulkan;
}

void bongo_cat_renderer_select_use_vulkan(bool enabled) {
    host_component_uses_vulkan = enabled;
}

#include <stdio.h>
#include <string.h>

/* 目录服务要用到的 app。见文件头。 */
static const BongoCatApp *host_app;

void bongo_cat_renderer_module_bind_app(const BongoCatApp *app) {
    host_app = app;
}

static void *service_core_symbol(const char *name) {
    return bongo_cat_live2d_core_symbol(name);
}

static void service_log(int priority, const char *message) {
    SDL_LogMessage(SDL_LOG_CATEGORY_APPLICATION, (SDL_LogPriority)priority,
        "[live2d] %s", message ? message : "");
}

static void service_log_format(int priority, const char *format,
    const char *argument) {
    /* 组件给的是**一条**字符串参数而不是可变参数: 跨 DLL 传 va_list 在 Windows 上不可移植
       (不同 CRT 的 va_list 布局不保证一致)。所以这里只支持 "%s" 形态, 复杂拼接由组件
       自己用 allocate() 拼好再走 log()。 */
    SDL_LogMessage(SDL_LOG_CATEGORY_APPLICATION, (SDL_LogPriority)priority,
        "[live2d] %s", argument && format ? argument : (format ? format : ""));
}

static void *service_allocate(size_t size) {
    return size ? SDL_malloc(size) : NULL;
}

static void service_release(void *memory) { SDL_free(memory); }

static bool service_directory(int kind, char *output, size_t capacity) {
    /* 六个目录直接问 app: 组件不该知道"六棵树"的布局, 更不该自己拼路径。 */
    const BongoCatApp *app = host_app;
    if (!app || !output || !capacity) return false;
    const char *root = NULL;
    switch (kind) {
    case BONGO_CAT_RENDERER_DIR_CONFIG: root = app->config_root; break;
    case BONGO_CAT_RENDERER_DIR_DATA: root = app->data_root; break;
    case BONGO_CAT_RENDERER_DIR_MODELS: root = app->models_root; break;
    case BONGO_CAT_RENDERER_DIR_CACHE: root = app->cache_root; break;
    case BONGO_CAT_RENDERER_DIR_STATE: root = app->state_root; break;
    case BONGO_CAT_RENDERER_DIR_LOGS: root = app->log_root; break;
    default: return false;
    }
    if (!root || !root[0]) return false;
    int written = snprintf(output, capacity, "%s", root);
    return written > 0 && (size_t)written < capacity;
}

static uint64_t service_ticks_ns(void) { return SDL_GetTicksNS(); }

static uint32_t service_active_backend(void) {
    return bongo_cat_renderer_active() == BONGO_CAT_RENDERER_VULKAN
        ? BONGO_CAT_RENDERER_BACKEND_VULKAN
        : BONGO_CAT_RENDERER_BACKEND_OPENGL;
}

static bool service_gl_context_ready(void) {
    /* 只要宿主已经建起 GL 上下文就算就绪: 组件不需要知道窗口/上下文的细节。 */
    return SDL_GL_GetCurrentContext() != NULL;
}

/* 上下文句柄以不透明指针交出: 组件只做"相等比较 + 原样回传", 解引用是它自己的错。 */
static void *service_gl_current_window(void) { return SDL_GL_GetCurrentWindow(); }
static void *service_gl_current_context(void) { return SDL_GL_GetCurrentContext(); }

static bool service_gl_make_current(void *window, void *context) {
    return SDL_GL_MakeCurrent((SDL_Window *)window, (SDL_GLContext)context);
}

static bool service_window_size_pixels(void *window, int *width, int *height) {
    if (!window) return false;
    return SDL_GetWindowSizeInPixels((SDL_Window *)window, width, height);
}

static bool service_gl_clear_errors(void) { return bongo_cat_gl_clear_errors(); }

/* 图像与 JSON 直接转发: 这两条管线带缓存与兼容性归一化, 只能有一份实现。
   注意 BongoCatImage 里的 SDL_Surface* 属于宿主, 组件只能读 pixels/width/height,
   释放必须回到 image_free。 */
static BongoCatResult service_image_fit(const char *path, int max_width,
    int max_height, BongoCatImage *out, BongoCatError *error) {
    return bongo_cat_image_fit(path, max_width, max_height, out, error);
}

static void service_image_free(BongoCatImage *image) {
    bongo_cat_image_free(image);
}

static void service_image_make_alpha_mask(const BongoCatImage *image,
    BongoCatImageAlphaMask *mask) {
    bongo_cat_image_make_alpha_mask(image, mask);
}

static unsigned int service_image_texture_model(const char *path,
    bool direct_decode, int *width, int *height, BongoCatImageAlphaMask *alpha,
    BongoCatImageProgress progress, void *userdata, BongoCatError *error) {
    return bongo_cat_image_texture_model(path, direct_decode, width, height,
        alpha, progress, userdata, error);
}

static yyjson_doc *service_model_json_parse(const char *data, size_t size,
    bool *normalized) {
    return bongo_cat_model_json_parse(data, size, normalized);
}

/* 读整个文件: 用 SDL_LoadFile 保证"二进制 + UTF-8 路径"在 Windows 上也正确,
   缓冲由这里分配, 组件用 release 归还(它可能就是 SDL_free, 但组件不该假设)。 */
static void *service_read_file(const char *path, size_t *size) {
    if (size) *size = 0;
    if (!path || !path[0]) return NULL;
    size_t length = 0;
    void *data = SDL_LoadFile(path, &length);
    if (!data) return NULL;
    if (size) *size = length;
    return data;
}

/* Vulkan 能力面: 只把 POD 快照交出去(device/queue/命令池句柄), 宿主私有的 BongoCatVk*
   绝不跨界 —— 这正是 2d 要解决的边界问题。 */
static bool service_vulkan_native_info(BongoCatVkNativeInfo *info) {
    /* 宿主私有的 BongoCatVk* 只在这一行里出现, 交出去的只是 POD 快照。 */
    const BongoCatVk *vk = bongo_cat_vk_current();
    return vk && info && bongo_cat_vk_native_info(vk, info);
}

static const BongoCatRendererModuleHost host_services = {
    BONGO_CAT_RENDERER_MODULE_ABI, sizeof(BongoCatRendererModuleHost),
    service_core_symbol, service_log, service_log_format, service_allocate,
    service_release, service_directory,
    service_ticks_ns, service_active_backend,
    bongo_cat_renderer_select_uses_vulkan, bongo_cat_renderer_select_use_vulkan,
    service_gl_context_ready, service_gl_clear_errors,
    service_gl_current_window, service_gl_current_context, service_gl_make_current,
    service_window_size_pixels,
    service_image_fit, service_image_free, service_image_make_alpha_mask,
    service_image_texture_model, service_model_json_parse, service_read_file,
    service_vulkan_native_info
};

const BongoCatRendererModuleHost *bongo_cat_renderer_module_host(void) {
    return &host_services;
}

bool bongo_cat_renderer_module_validate_host(
    const BongoCatRendererModuleHost *host) {
    /* 组件侧要做的同一件事(纪律 2): 宿主的表头对不上就别往下走。这里是宿主自查,
       保证我们交给组件的表永远是自洽的 —— 少一项服务就等于组件会在运行期拿到空指针。 */
    return host && host->abi_version == BONGO_CAT_RENDERER_MODULE_ABI &&
        host->struct_size == sizeof(*host) && host->core_symbol && host->log &&
        host->log_format && host->allocate && host->release && host->directory &&
        host->ticks_ns && host->active_backend &&
        host->cubism_renderer_uses_vulkan && host->cubism_renderer_set_vulkan &&
        host->gl_context_ready && host->gl_clear_errors && host->gl_current_window &&
        host->gl_current_context && host->gl_make_current && host->window_size_pixels &&
        host->image_fit &&
        host->image_free && host->image_make_alpha_mask &&
        host->image_texture_model && host->model_json_parse && host->read_file &&
        host->vulkan_native_info;
}
