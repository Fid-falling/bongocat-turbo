/*
 * 渲染组件 ABI 的"坏描述符矩阵"。
 *
 * 断言两件事, 而不是"能加载就行":
 *   1. 每一种坏形态都被**明确拒绝**, 并且拒绝码/原因说得出是哪里不对(用户与组件作者都要
 *      靠这句话定位问题);
 *   2. 校验是**纯函数**: 不需要真实渲染器、不需要图形上下文、不需要 Core 就能得出结论 ——
 *      所以它可以进 CI 常态跑, 而不是靠人工在真机上试。
 *
 * 坏形态由 tests/plugin/test_component.c 这个"会说谎的组件"提供(它只在测试里构建)。
 * 另外还测加载器对"根本不是组件"的文件的处理: 缺导出符号时必须失败且给出可读原因。
 */
#include "bongo_cat/live2d_core.h"
#include "bongo_cat/path.h"
#include "bongo_cat/renderer_module.h"
#include "bongo_cat/renderer_module_abi.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <string.h>

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

static int failures;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, message); \
        ++failures; \
    } \
} while (0)

typedef void (*ConfigureFn)(int);
typedef const BongoCatRendererModule *(*QueryFn)(uint32_t,
    const BongoCatRendererModuleHost *);

/* 期望: 校验失败且原因里含 needle(大小写敏感, 便于确认报的是那一条而不是别的原因)。 */
static void expect_rejected(QueryFn query, ConfigureFn configure, int mode,
    BongoCatResult expected, const char *needle, const char *label) {
    configure(mode);
    const BongoCatRendererModule *module = query(BONGO_CAT_RENDERER_MODULE_ABI,
        bongo_cat_renderer_module_host());
    BongoCatResult result = BONGO_CAT_OK;
    char reason[BONGO_CAT_RENDERER_MODULE_REASON_CAP] = {0};
    bool accepted = bongo_cat_renderer_module_validate(module,
        bongo_cat_renderer_module_host(), &result, reason, sizeof(reason));
    CHECK(!accepted, label);
    CHECK(result == expected, label);
    if (needle)
        CHECK(strstr(reason, needle) != NULL, label);
    if (accepted || (needle && !strstr(reason, needle)))
        fprintf(stderr, "  (%s) reason was: %s\n", label, reason);
}

int main(int argc, char **argv) {
    /* 夹具路径由测试命令传入(见 cmake/Tests.cmake): 用命令行而不是编译期宏, 免得 Windows
       原生路径里的反斜杠被当成 C 转义序列。 */
    const char *component_path = argc > 1 ? argv[1] : NULL;
    if (!component_path || !component_path[0]) {
        fprintf(stderr, "usage: %s <path to the test component>\n",
            argc > 0 ? argv[0] : "test_renderer_module_abi");
        return 1;
    }
    /* 宿主服务表必须自洽(它是交给组件的唯一入口)。 */
    CHECK(bongo_cat_renderer_module_validate_host(
        bongo_cat_renderer_module_host()), "the host service table must be valid");

    SDL_SharedObject *object = SDL_LoadObject(component_path);
    if (!object) {
        fprintf(stderr, "cannot load the test component: %s\n", SDL_GetError());
        return 1;
    }
    ConfigureFn configure = (ConfigureFn)SDL_LoadFunction(object,
        "bongo_cat_test_component_configure");
    QueryFn query = (QueryFn)SDL_LoadFunction(object,
        "bongo_cat_renderer_module_query");
    CHECK(configure != NULL, "the test component must expose the configure hook");
    CHECK(query != NULL, "the test component must expose the query symbol");
    if (!configure || !query) {
        SDL_UnloadObject(object);
        return 1;
    }

    /* —— 正例 —— */
    configure(TEST_COMPONENT_VALID);
    const BongoCatRendererModule *valid = query(BONGO_CAT_RENDERER_MODULE_ABI,
        bongo_cat_renderer_module_host());
    BongoCatResult result = BONGO_CAT_ERROR_CUBISM;
    char reason[BONGO_CAT_RENDERER_MODULE_REASON_CAP] = {0};
    CHECK(bongo_cat_renderer_module_validate(valid,
        bongo_cat_renderer_module_host(), &result, reason, sizeof(reason)),
        "a well-formed descriptor must be accepted");
    CHECK(result == BONGO_CAT_OK, "a valid descriptor reports OK");

    /* —— 反例矩阵 —— */
    expect_rejected(query, configure, TEST_COMPONENT_BAD_ABI,
        BONGO_CAT_ERROR_FORMAT, "ABI version", "an ABI mismatch must be rejected");
    expect_rejected(query, configure, TEST_COMPONENT_BAD_STRUCT_SIZE,
        BONGO_CAT_ERROR_FORMAT, "size", "struct_size drift must be rejected");
    expect_rejected(query, configure, TEST_COMPONENT_MISSING_OP,
        BONGO_CAT_ERROR_FORMAT, "draw()",
        "a missing required callback must be named in the reason");
    expect_rejected(query, configure, TEST_COMPONENT_UNKNOWN_ENGINE,
        BONGO_CAT_ERROR_FORMAT, "engine", "an unknown engine must be rejected");
    expect_rejected(query, configure, TEST_COMPONENT_BAD_BACKEND,
        BONGO_CAT_ERROR_PLATFORM, "backend",
        "an unsupported graphics backend must be rejected");
    /* 没有 Core 时, "需要 Core"的组件必须被拒, 且原因指向 Core(而不是含糊的失败)。 */
    if (!bongo_cat_live2d_core_available()) {
        expect_rejected(query, configure, TEST_COMPONENT_REQUIRES_CORE,
            BONGO_CAT_ERROR_CUBISM, "Cubism Core",
            "REQUIRES_CORE without an installed Core must be rejected");
    } else {
        configure(TEST_COMPONENT_REQUIRES_CORE);
        CHECK(bongo_cat_renderer_module_validate(query(BONGO_CAT_RENDERER_MODULE_ABI,
            bongo_cat_renderer_module_host()), bongo_cat_renderer_module_host(),
            &result, reason, sizeof(reason)),
            "with a Core installed the same component must be accepted");
    }

    /* 组件自己拒绝宿主(返回 NULL)时, 校验必须给出可读原因而不是崩。 */
    configure(TEST_COMPONENT_RETURNS_NULL);
    const BongoCatRendererModule *empty = query(BONGO_CAT_RENDERER_MODULE_ABI,
        bongo_cat_renderer_module_host());
    CHECK(empty == NULL, "the component may refuse the host by returning NULL");
    CHECK(!bongo_cat_renderer_module_validate(empty,
        bongo_cat_renderer_module_host(), &result, reason, sizeof(reason)),
        "a NULL descriptor must be rejected");
    CHECK(strstr(reason, "no descriptor") != NULL,
        "a NULL descriptor must explain itself");

    /* 组件侧也要校验宿主: ABI 不匹配时必须拿到 NULL(而不是被读越界)。 */
    CHECK(query(BONGO_CAT_RENDERER_MODULE_ABI + 1u,
        bongo_cat_renderer_module_host()) == NULL,
        "the component must refuse a host with a different ABI");

    /* 加载器对"拒绝宿主的组件"的处理: 必须失败、必须说得出原因、且不得留下活动组件。
       注意这里**不能**先 SDL_UnloadObject: 卸载会把夹具模块的状态清掉(重新加载后它又变成
       好组件), 那样测的就不是这条路径了。 */
    BongoCatError error = {0};
    CHECK(!bongo_cat_renderer_module_open(component_path, NULL, &error),
        "loading a component that refuses the host must fail");
    CHECK(error.message[0] != '\0', "the rejection must explain itself");
    CHECK(!bongo_cat_renderer_module_active(),
        "a failed load must not leave an active component");
    SDL_UnloadObject(object);

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("renderer module ABI matrix passed");
    return 0;
}

