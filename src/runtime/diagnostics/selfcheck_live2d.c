/*
 * 自检第 3 组: Live2D。
 *
 * 用户的日志停在 "[live2d/vk] create_renderer: uses_vulkan=1 bridge_ready=1" —— 那一行
 * 正是本进程里最后一次"Live2D 要开始建渲染器"的记录, 之后再没有任何输出。所以这一组
 * 的第三项必须回答同一个问题: 组件渲染器到底建出来了没有。
 *
 * 这里的"探测"是一个**只读的状态判定**, 不是再建一个渲染器: SDK 的 create_renderer 绑定在
 * 一个已加载的模型上 (它要读 drawable/mask 数量、要挂贴图), 在运行中复制一份既没有意义
 * 也会干扰正在渲染的宠物。真正的事实来源是: 组件是否装载、Core 是否可用、当前模型是否
 * 由组件原生渲染 (native_active) —— 三者同时为真就等价于"create_renderer 已经成功过"。
 * 用户那种"卡在 create_renderer"的机器, 这里必然是失败, 并直接指出卡在哪一层。
 */
#include "selfcheck_internal.h"

#include "bongo_cat/app.h"
#include "bongo_cat/live2d_core.h"
#include "bongo_cat/live2d_vulkan.h"
#include "bongo_cat/log.h"
#include "bongo_cat/path.h"
#include "bongo_cat/render.h"
#include "bongo_cat/renderer_module.h"
#include "bongo_cat/sha256.h"
#include "live2d_dispatch.h"

#include <stdio.h>
#include <string.h>

/* 组件扫描结果里找出 Live2D 组件 (名字以 bongo_live2d 开头, 不含扩展名约束) */
static const BongoCatRendererModuleInfo *find_live2d_component(void) {
    BongoCatRendererModuleInfo *entries = NULL;
    size_t count = bongo_cat_renderer_module_last(&entries);
    for (size_t i = 0; i < count; ++i) {
        if (strncmp(entries[i].name, BONGO_CAT_RENDERER_MODULE_LIVE2D,
                strlen(BONGO_CAT_RENDERER_MODULE_LIVE2D)) == 0)
            return &entries[i];
    }
    return NULL;
}

static void component_item(BongoCatSelfCheckBuilder *builder) {
    const BongoCatRendererModuleInfo *component = find_live2d_component();
    if (!component) {
        char directory[BONGO_CAT_PATH_CAP];
        bool exists = false;
        bongo_cat_renderer_module_directory(directory, sizeof(directory), &exists);
        bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_FAIL, "渲染组件",
            "未找到 Live2D 组件 (预期目录: %s%s)。可重新安装或把 cores/ 放回"
            "程序目录", directory, exists ? "" : "，该目录当前不存在");
        return;
    }
    char hash[65] = {0};
    BongoCatError error = {0};
    uint64_t size = 0;
    bool hashed = bongo_cat_path_file_size(component->path, &size) &&
        bongo_cat_sha256_file(component->path, hash, &error) == BONGO_CAT_OK;
    char digest[24];
    snprintf(digest, sizeof(digest), "%s", hashed ? hash : "无法计算");
    if (hashed && strlen(hash) > 16) digest[16] = '\0';
    switch (component->state) {
    case BONGO_CAT_RENDERER_MODULE_READY:
        bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_PASS, "渲染组件",
            "%s (%llu 字节) · SHA-256 %s… · 可加载", component->name,
            (unsigned long long)size, digest);
        break;
    case BONGO_CAT_RENDERER_MODULE_DISABLED:
        bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_WARN, "渲染组件",
            "%s 被 .disabled 标记停用，Live2D 将使用诊断兜底实现（删掉同名"
            " .disabled 文件即可恢复）", component->name);
        break;
    default:
        bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_FAIL, "渲染组件",
            "%s 无法加载：%s", component->name,
            component->reason[0] ? component->reason : "ABI/引擎校验失败");
        break;
    }
}

static void core_item(BongoCatSelfCheckBuilder *builder) {
    if (!bongo_cat_live2d_core_available()) {
        bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_FAIL, "Cubism Core",
            "未导入 Cubism Core：Live2D 模型无法加载（设置 → 互动 → Live2D "
            "可导入官方 SDK 压缩包）");
        return;
    }
    bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_PASS, "Cubism Core",
        "已导入：%s · csm 版本 0x%08X", bongo_cat_live2d_core_loaded_path(),
        (unsigned)bongo_cat_live2d_core_version());
}

/* 第三项: 渲染器到底建出来了没有 (对应日志里 create_renderer 那一行之后是否还有输出) */
static void renderer_item(BongoCatSelfCheckBuilder *builder,
    struct BongoCatApp *app) {
    const char *model = app && app->loaded_model[0] ? app->loaded_model : NULL;
    const bool native = bongo_cat_live2d_native_active();
    const bool vk_backend =
        bongo_cat_renderer_active() == BONGO_CAT_RENDERER_VULKAN;
    const bool vk_ready = bongo_cat_live2d_vulkan_ready();
    if (!bongo_cat_live2d_core_available() || !find_live2d_component()) {
        bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_FAIL,
            "渲染器创建 (create_renderer)",
            "前置条件不满足（组件或 Core 不可用），create_renderer 必然失败");
        return;
    }
    if (!model) {
        bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_WARN,
            "渲染器创建 (create_renderer)",
            "当前没有已加载的模型，无法确认 create_renderer 的结果；"
            "先加载一个模型再运行一次自检");
        return;
    }
    /*
     * 内置模型是 PNG 叠加图, 根本不走 Cubism 渲染器: 对它们报"渲染器没建出来"是误报。
     * 只有能力的 LIVE2D 位为真的模型才会走到 create_renderer —— 用户报的那台机器上
     * 正是这种模型 (日志里那一行 [live2d/vk] create_renderer)。
     */
    const BongoCatModelEntry *entry = bongo_cat_models_find(&app->models, model);
    if (!entry || !(entry->capabilities & BONGO_CAT_MODEL_CAPABILITY_LIVE2D)) {
        bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_WARN,
            "渲染器创建 (create_renderer)",
            "当前模型 %s 不是 Live2D 模型（内置 PNG 资源），不需要组件渲染器；"
            "要验证 create_renderer 请先切换到 Live2D 模型", model);
        return;
    }
    if (!native) {
        bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_FAIL,
            "渲染器创建 (create_renderer)",
            "模型 %s 没有由组件原生渲染（当前是诊断兜底实现）：后端=%s，"
            "Vulkan 桥=%s。这正是日志停在 create_renderer 的那一层失败",
            model, bongo_cat_renderer_name(bongo_cat_renderer_active()),
            vk_ready ? "已就绪" : "未就绪");
        return;
    }
    if (vk_backend && !vk_ready)
        bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_FAIL,
            "渲染器创建 (create_renderer)",
            "模型 %s 已由组件渲染，但 Vulkan 桥未就绪（合成阶段会失败）",
            model);
    else
        bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_PASS,
            "渲染器创建 (create_renderer)",
            "已成功：模型 %s 由组件原生渲染 (backend=%s, Vulkan 桥=%s)",
            model, bongo_cat_renderer_name(bongo_cat_renderer_active()),
            vk_ready ? "就绪" : "未使用");
}

void bongo_cat_selfcheck_live2d(BongoCatSelfCheckBuilder *builder,
    struct BongoCatApp *app) {
    bongo_cat_selfcheck_section(builder, "Live2D");
    component_item(builder);
    core_item(builder);
    renderer_item(builder, app);
#ifdef BONGO_CAT_HAS_VULKAN
    if (bongo_cat_renderer_compiled_in(BONGO_CAT_RENDERER_VULKAN)) {
        const char *assets = app ? app->asset_root : "";
        if (bongo_cat_live2d_vulkan_shaders_available(assets))
            bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_PASS,
                "Vulkan 着色器",
                "FrameworkShaders/*.spv 已找到（%s）", assets);
        else
            bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_FAIL,
                "Vulkan 着色器",
                "缺少 FrameworkShaders/*.spv（查找根: %s）：Vulkan 下 Live2D "
                "会画成破碎画面，请把 Release 目录里的 FrameworkShaders 文件夹"
                "和 exe 放在一起", assets);
    }
#endif
    if (app && app->loaded_model[0])
        bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_PASS, "当前模型",
            "%s（模型目录共 %zu 个）", app->loaded_model, app->models.count);
    else
        bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_WARN, "当前模型",
            "没有已加载的模型（模型目录共 %zu 个）",
            app ? app->models.count : (size_t)0);
}
