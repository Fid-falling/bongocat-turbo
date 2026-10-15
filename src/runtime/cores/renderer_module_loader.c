/*
 * 渲染组件的加载与 ABI 校验(宿主侧)。
 *
 * 分两层, 刻意如此:
 *   * `bongo_cat_renderer_module_validate()` 是**纯函数** —— 给它一份描述符与宿主服务表,
 *     它回答"能不能用, 不能是为什么"。因为它不碰文件系统、不加载动态库, 所以"坏描述符矩阵"
 *     可以逐条断言(见 tests/plugin/test_renderer_module_abi.c), 不需要构造真实组件。
 *   * `bongo_cat_renderer_module_open()` 负责脏活: dlopen、取导出符号、调用 query、校验、
 *     实例化、以及失败时把已经拿到的东西**按相反顺序**放回去。
 *
 * 校验顺序是有意的: 描述符形状 → 引擎 → 后端 → Core → 必备回调。
 * 把 Core 放在后端之后: 用户拿到"缺 Core"才去官网下载, 而如果真正的问题是这次构建不支持
 * 他的显卡后端, 先报缺 Core 会让他白下载一次(几百 MB)还解决不了问题。
 *
 * 卸载纪律: 必须先 destroy 实例(它会释放 GPU 资源), 再释放动态库。顺序颠倒会让插件的
 * 析构代码在自己的代码段已被卸载后执行 —— 那是最难查的一类崩溃。
 */
#include "bongo_cat/live2d_core.h"
#include "bongo_cat/log.h"
#include "bongo_cat/renderer_module.h"
#include "bongo_cat/renderer_module_abi.h"
#include "bongo_cat/render.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <string.h>

/* 组件是否支持当前生效的后端。宿主选择的后端由 render 模块决定(Auto 已经解析过),
   所以这里问的是"现在真正在跑的那个"。 */
static uint32_t active_backend_flag(void) {
    return bongo_cat_renderer_active() == BONGO_CAT_RENDERER_VULKAN
        ? BONGO_CAT_RENDERER_BACKEND_VULKAN
        : BONGO_CAT_RENDERER_BACKEND_OPENGL;
}

/* 必备回调逐个点名: 报"缺哪个"比报"描述符无效"有用得多(组件作者一眼就知道要补什么)。 */
static const char *missing_required_op(const BongoCatRendererModuleOps *ops) {
    if (!ops->create) return "create";
    if (!ops->destroy) return "destroy";
    if (!ops->load) return "load";
    if (!ops->ready) return "ready";
    if (!ops->update) return "update";
    if (!ops->draw) return "draw";
    if (!ops->resize) return "resize";
    return NULL;
}

bool bongo_cat_renderer_module_validate(const BongoCatRendererModule *module,
    const BongoCatRendererModuleHost *host, BongoCatResult *result,
    char *reason, size_t capacity) {
    if (result) *result = BONGO_CAT_ERROR_CUBISM;
#define REJECT(code, text) do { \
        if (result) *result = (code); \
        if (reason && capacity) snprintf(reason, capacity, "%s", (text)); \
        return false; \
    } while (0)
    if (!module) REJECT(BONGO_CAT_ERROR_FORMAT, "the component returned no descriptor");
    if (!host) REJECT(BONGO_CAT_ERROR_ARGUMENT, "no host service table");
    /* 描述符形状: 先看这两个, 再看别的 —— 其余字段的偏移只有在它们匹配时才有意义。 */
    if (module->abi_version != BONGO_CAT_RENDERER_MODULE_ABI)
        REJECT(BONGO_CAT_ERROR_FORMAT,
            "the component was built for a different ABI version");
    if (module->struct_size != sizeof(*module))
        REJECT(BONGO_CAT_ERROR_FORMAT,
            "the component descriptor has a different size (ABI drift)");
    if (module->engine != BONGO_CAT_RENDERER_ENGINE_LIVE2D)
        REJECT(BONGO_CAT_ERROR_FORMAT, "the component implements an unknown engine");
    /* 能力位协商: 后端先于 Core(理由见文件头)。 */
    if (!(module->graphics_backends & active_backend_flag()))
        REJECT(BONGO_CAT_ERROR_PLATFORM,
            "the component does not support the active graphics backend");
    if ((module->flags & BONGO_CAT_RENDERER_FLAG_REQUIRES_CORE) &&
        !bongo_cat_live2d_core_available())
        REJECT(BONGO_CAT_ERROR_CUBISM,
            "the component requires the user-supplied Cubism Core, which is not "
            "installed yet");
    const char *missing = missing_required_op(&module->ops);
    if (missing) {
        if (reason && capacity)
            snprintf(reason, capacity, "the component does not implement %s()", missing);
        if (result) *result = BONGO_CAT_ERROR_FORMAT;
        return false;
    }
#undef REJECT
    if (reason && capacity) reason[0] = '\0';
    if (result) *result = BONGO_CAT_OK;
    return true;
}

/* 当前装载的组件(同一时刻只允许一个 Live2D 渲染组件: 两个实现抢同一块画布没有意义,
   而且会让"用哪个实现渲染"变成不可预测的事)。 */
typedef struct BongoCatRendererModuleSlot {
    SDL_SharedObject *object;
    const BongoCatRendererModule *module;
    void *instance;
    char path[BONGO_CAT_PATH_CAP];
} BongoCatRendererModuleSlot;

static BongoCatRendererModuleSlot slot;

bool bongo_cat_renderer_module_open(const char *path,
    const char *asset_root, BongoCatError *error) {
    if (!path || !path[0]) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_ARGUMENT,
            "Cannot load a renderer component without a path");
        return false;
    }
    if (slot.object) bongo_cat_renderer_module_close();
    if (!bongo_cat_renderer_module_validate_host(bongo_cat_renderer_module_host())) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "The host service table is inconsistent; refusing to load a component");
        return false;
    }
    SDL_LoadObject(NULL); /* 清掉上一次的 SDL 错误 */
    SDL_SharedObject *object = SDL_LoadObject(path);
    if (!object) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO, "%s", SDL_GetError());
        return false;
    }
    BongoCatRendererModuleQuery query = (BongoCatRendererModuleQuery)
        SDL_LoadFunction(object, BONGO_CAT_RENDERER_MODULE_SYMBOL);
    if (!query) {
        SDL_UnloadObject(object);
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            "Not a BongoCat renderer component (missing %s)",
            BONGO_CAT_RENDERER_MODULE_SYMBOL);
        return false;
    }
    const BongoCatRendererModule *module = query(BONGO_CAT_RENDERER_MODULE_ABI,
        bongo_cat_renderer_module_host());
    char reason[BONGO_CAT_RENDERER_MODULE_REASON_CAP] = {0};
    BongoCatResult result = BONGO_CAT_ERROR_CUBISM;
    if (!bongo_cat_renderer_module_validate(module, bongo_cat_renderer_module_host(), &result, reason,
            sizeof(reason))) {
        SDL_UnloadObject(object);
        bongo_cat_error_set(error, result, "%s", reason);
        return false;
    }
    /* 校验通过才建实例: 让"不匹配的组件"在创建任何资源之前就被挡住 —— 否则失败的 create
       可能已经申请了 GPU 资源, 而它的 destroy 未必被调用。 */
    void *instance = module->ops.create(bongo_cat_renderer_module_host(), asset_root, error);
    if (!instance) {
        SDL_UnloadObject(object);
        if (error && !error->message[0])
            bongo_cat_error_set(error, BONGO_CAT_ERROR_CUBISM,
                "The renderer component failed to create an instance");
        return false;
    }
    slot.object = object;
    slot.module = module;
    slot.instance = instance;
    snprintf(slot.path, sizeof(slot.path), "%s", path);
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[live2d] 已加载渲染组件 %s (引擎 %u, 后端位 0x%x, 需要 Core: %s)",
        module->name ? module->name : "(unnamed)", (unsigned)module->engine,
        (unsigned)module->graphics_backends,
        (module->flags & BONGO_CAT_RENDERER_FLAG_REQUIRES_CORE) ? "是" : "否");
    return true;
}

bool bongo_cat_renderer_module_active(void) { return slot.instance != NULL; }

const char *bongo_cat_renderer_module_active_path(void) { return slot.path; }

/* 卸载: 先销毁实例(释放 GPU 资源), 再释放库。顺序见文件头。 */
void bongo_cat_renderer_module_close(void) {
    if (slot.instance && slot.module && slot.module->ops.destroy)
        slot.module->ops.destroy(slot.instance);
    slot.instance = NULL;
    slot.module = NULL;
    if (slot.object) SDL_UnloadObject(slot.object);
    slot.object = NULL;
    if (slot.path[0])
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE, "[live2d] 已卸载渲染组件: %s", slot.path);
    slot.path[0] = '\0';
}
