/*
 * 测试用的"渲染组件": 它**不是**真的渲染器, 而是一个可以按需说谎的描述符工厂。
 *
 * 为什么需要它: ABI 校验的价值全在"坏输入被挡住"这一侧, 而坏输入无法用真组件构造。这里把
 * 各种坏形态做成 mode, 由 tests/plugin/test_renderer_module_abi.c 逐条断言校验函数拒绝
 * 它们并且**拒绝得有理有据**(result 码 + reason 文案)。
 *
 * 这个目标只被测试构建, 不随发行包分发(见 cmake/Tests.cmake)。
 */
#include "bongo_cat/renderer_module_abi.h"

#include <string.h>

/* mode 定义与测试侧共用: 任何改动都要同步 tests/plugin/test_renderer_module_abi.c。 */
enum {
    TEST_COMPONENT_VALID = 0,
    TEST_COMPONENT_BAD_ABI = 1,
    TEST_COMPONENT_BAD_STRUCT_SIZE = 2,
    TEST_COMPONENT_MISSING_OP = 3,
    TEST_COMPONENT_REQUIRES_CORE = 4,
    TEST_COMPONENT_BAD_BACKEND = 5,
    TEST_COMPONENT_RETURNS_NULL = 6,
    TEST_COMPONENT_UNKNOWN_ENGINE = 7
};

static int component_mode;

/* 宿主实际上不会走到这些回调(测试只校验描述符), 但必备回调必须非空, 所以给出最小实现。 */
static void *op_create(const BongoCatRendererModuleHost *host,
    const char *asset_root, BongoCatError *error) {
    (void)host; (void)asset_root; (void)error;
    return &component_mode; /* 非 NULL 即可: 测试不会 destroy 它之外的东西 */
}
static void op_destroy(void *instance) { (void)instance; }
static BongoCatResult op_load(void *instance, const char *model_dir,
    const char *setting_file, bool preset,
    const BongoCatLive2DRenderOptions *render_options,
    BongoCatLive2DLoadProgress progress, void *userdata, BongoCatError *error) {
    (void)instance; (void)model_dir; (void)setting_file; (void)preset;
    (void)render_options; (void)progress; (void)userdata; (void)error;
    return BONGO_CAT_OK;
}
static bool op_ready(const void *instance) { (void)instance; return true; }
static bool op_update(void *instance, float delta_seconds) {
    (void)instance; (void)delta_seconds; return false;
}
static void op_draw(void *instance) { (void)instance; }
static void op_resize(void *instance, int width, int height) {
    (void)instance; (void)width; (void)height;
}

/* 测试入口: 切到某个坏形态。SDL_LoadFunction 取它的地址后调用。 */
BONGO_CAT_RENDERER_MODULE_EXPORT void bongo_cat_test_component_configure(int mode) { component_mode = mode; }

BONGO_CAT_RENDERER_MODULE_EXPORT const BongoCatRendererModule *bongo_cat_renderer_module_query(uint32_t abi,
    const BongoCatRendererModuleHost *host) {
    /* 组件侧也要校验宿主(纪律 2): 表头对不上就干净地返回 NULL, 而不是读越界。 */
    if (abi != BONGO_CAT_RENDERER_MODULE_ABI || !host ||
        host->abi_version != abi || host->struct_size != sizeof(*host))
        return NULL;
    if (component_mode == TEST_COMPONENT_RETURNS_NULL) return NULL;
    static BongoCatRendererModule module;
    memset(&module, 0, sizeof(module));
    module.abi_version = BONGO_CAT_RENDERER_MODULE_ABI;
    module.struct_size = sizeof(module);
    module.engine = BONGO_CAT_RENDERER_ENGINE_LIVE2D;
    module.graphics_backends = BONGO_CAT_RENDERER_BACKEND_OPENGL |
        BONGO_CAT_RENDERER_BACKEND_VULKAN;
    module.name = "test component";
    module.ops.create = op_create;
    module.ops.destroy = op_destroy;
    module.ops.load = op_load;
    module.ops.ready = op_ready;
    module.ops.update = op_update;
    module.ops.draw = op_draw;
    module.ops.resize = op_resize;
    switch (component_mode) {
    case TEST_COMPONENT_BAD_ABI:
        module.abi_version = BONGO_CAT_RENDERER_MODULE_ABI + 7u;
        break;
    case TEST_COMPONENT_BAD_STRUCT_SIZE:
        module.struct_size = sizeof(module) + 16u;
        break;
    case TEST_COMPONENT_MISSING_OP:
        module.ops.draw = NULL;
        break;
    case TEST_COMPONENT_REQUIRES_CORE:
        module.flags = BONGO_CAT_RENDERER_FLAG_REQUIRES_CORE;
        break;
    case TEST_COMPONENT_BAD_BACKEND:
        module.graphics_backends = 0x100u; /* 谁都不认识的后端位 */
        break;
    case TEST_COMPONENT_UNKNOWN_ENGINE:
        module.engine = 99u;
        break;
    default:
        break;
    }
    return &module;
}
