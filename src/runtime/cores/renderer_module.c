/*
 * Live2D 渲染组件(`bongo_live2d`)的目录解析与启动扫描(宿主侧)。
 *
 * 只做三件事: 找到目录 → 枚举候选库 → 逐个判定"能不能用"(停用标记 / 可加载 / 导出我们的
 * 查询符号)并写日志。**不实例化、不调用组件代码** —— 描述符校验与 create 在阶段 2 的加载器
 * 里做, 因为那需要实例与 GPU 资源的生命周期配合。
 *
 * 为什么"发现"要独立成一层: 用户排障时最需要知道的是"程序到底看没看见我的组件、为什么不用
 * 它"。把这三态(没找到 / 被停用 / 拒绝加载 + 原因)写进日志与 UI, 比让用户猜要省一个
 * 支持工单。因此每一步都有明确的日志行, 且拒绝原因带具体信号(缺哪个导出符号 / SDL 的错)。
 */
#include "bongo_cat/renderer_module.h"
#include "bongo_cat/log.h"
#include "bongo_cat/path.h"

#include "storage_paths.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <string.h>

#define MODULE_ENTRY_LIMIT 8

/* 主机侧缓存: 扫描是启动期一次性的动作, 结果留着给 UI 用, 避免在渲染线程上重复扫盘。 */
static BongoCatRendererModuleInfo cached[MODULE_ENTRY_LIMIT];
static size_t cached_count;

/* 往缓存里追加一条并写日志。满了就丢弃并记一条警告: 组件目录里塞了十几个库不是正常用法,
   但也不该让启动失败。 */
static void record(BongoCatRendererModuleInfo *entries, size_t *count,
    size_t capacity, const char *name, const char *path,
    BongoCatRendererModuleState state, const char *reason) {
    BongoCatRendererModuleInfo info = {0};
    snprintf(info.name, sizeof(info.name), "%s", name);
    snprintf(info.path, sizeof(info.path), "%s", path);
    info.state = state;
    if (reason) snprintf(info.reason, sizeof(info.reason), "%s", reason);
    switch (state) {
    case BONGO_CAT_RENDERER_MODULE_READY:
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE, "[live2d] 发现渲染组件: %s", path);
        break;
    case BONGO_CAT_RENDERER_MODULE_DISABLED:
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[live2d] 组件被停用标记跳过(%s.disabled 存在): %s", name, path);
        break;
    default:
        SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE, "[live2d] 拒绝加载组件 %s: %s", path,
            reason ? reason : "unknown reason");
        break;
    }
    if (*count >= capacity) {
        SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
            "[live2d] 组件目录里的条目超过 %zu 个, 多余的条目被忽略",
            (size_t)capacity);
        return;
    }
    entries[(*count)++] = info;
}

/* 平台扩展名: 只接受动态库, 免得把 README.txt 也当成组件去 dlopen。 */
static bool library_name(const char *name) {
    static const char *const suffixes[] = {".dll", ".so", ".dylib"};
    size_t length = strlen(name);
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
        size_t suffix_length = strlen(suffixes[i]);
        if (length > suffix_length &&
            !SDL_strcasecmp(name + length - suffix_length, suffixes[i]))
            return true;
    }
    return false;
}

static void strip_suffix(char *stem, size_t capacity, const char *name) {
    snprintf(stem, capacity, "%s", name);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = '\0';
}

/* 判定一个候选文件: 停用标记 → 可加载 → 导出符号。
   这里刻意**不**深入解析组件结构体: 发现阶段的职责只是"能不能用 + 为什么", 而结构体校验
   必须与实例化顺序绑在一起(见阶段 2 的 ABI 校验)。 */
static void classify(BongoCatRendererModuleInfo *entries, size_t *count,
    size_t capacity, const char *directory, const char *name) {
    char path[BONGO_CAT_PATH_CAP], disabled[BONGO_CAT_PATH_CAP], marker[168];
    if (!bongo_cat_path_join(path, sizeof(path), directory, name)) return;
    char stem[128];
    strip_suffix(stem, sizeof(stem), name);
    if (snprintf(marker, sizeof(marker), "%s.disabled", stem) > 0 &&
        bongo_cat_path_join(disabled, sizeof(disabled), directory, marker) &&
        bongo_cat_path_is_file(disabled)) {
        record(entries, count, capacity, name, path,
            BONGO_CAT_RENDERER_MODULE_DISABLED, NULL);
        return;
    }
    SDL_LoadObject(NULL); /* 清掉上一次的 SDL 错误, 免得报出别人的原因 */
    SDL_SharedObject *object = SDL_LoadObject(path);
    if (!object) {
        record(entries, count, capacity, name, path,
            BONGO_CAT_RENDERER_MODULE_REJECTED, SDL_GetError());
        return;
    }
    SDL_FunctionPointer query = SDL_LoadFunction(object,
        BONGO_CAT_RENDERER_MODULE_SYMBOL);
    SDL_UnloadObject(object);
    if (!query) {
        record(entries, count, capacity, name, path,
            BONGO_CAT_RENDERER_MODULE_UNKNOWN,
            "missing the " BONGO_CAT_RENDERER_MODULE_SYMBOL " export");
        return;
    }
    record(entries, count, capacity, name, path, BONGO_CAT_RENDERER_MODULE_READY,
        NULL);
}

typedef struct ModuleScan {
    BongoCatRendererModuleInfo *entries;
    size_t *count;
    size_t capacity;
} ModuleScan;

static BongoCatPathVisit scan_visitor(void *userdata, const char *directory,
    const char *name) {
    ModuleScan *scan = userdata;
    if (library_name(name))
        classify(scan->entries, scan->count, scan->capacity, directory, name);
    return BONGO_CAT_PATH_CONTINUE;
}

/* 存储根未解析时退到 <exe>/cores: "把组件放在 exe 旁边"是发行包的默认形态, 必须一直可用。 */
static bool fallback_directory(char *output, size_t capacity, const char *child) {
    const char *base = SDL_GetBasePath();
    return base && bongo_cat_path_join(output, capacity, base, child);
}

bool bongo_cat_renderer_module_directory(char *output, size_t capacity,
    bool *exists) {
    if (!output || !capacity) return false;
    if (exists) *exists = false;
    const char *configured = SDL_getenv("BONGO_CAT_CORES_DIRECTORY");
    if (configured && configured[0]) {
        /* 环境变量覆盖时不再往下找: 用户显式指定的目录就是唯一来源, 否则"我明明设了"
           与"程序偷偷用了另一个"会同时成立, 排障时无法收敛。 */
        snprintf(output, capacity, "%s", configured);
        if (exists) *exists = bongo_cat_path_is_dir(output);
        return true;
    }
    char exe_cores[BONGO_CAT_PATH_CAP], storage_cores[BONGO_CAT_PATH_CAP];
    exe_cores[0] = '\0';
    storage_cores[0] = '\0';
    if (fallback_directory(exe_cores, sizeof(exe_cores), "cores") &&
        bongo_cat_path_is_dir(exe_cores)) {
        snprintf(output, capacity, "%s", exe_cores);
        if (exists) *exists = true;
        return true;
    }
    char root[BONGO_CAT_PATH_CAP];
    if (bongo_cat_storage_paths_root(root, sizeof(root)) &&
        bongo_cat_path_join(storage_cores, sizeof(storage_cores), root, "cores") &&
        bongo_cat_path_is_dir(storage_cores)) {
        snprintf(output, capacity, "%s", storage_cores);
        if (exists) *exists = true;
        return true;
    }
    /* 谁都没有: 返回"随包分发"的那个路径(它是我们要在打包检查里断言存在的投放点),
       并明确 exists=false, 让调用方知道现在没有可用组件。 */
    if (exe_cores[0]) {
        snprintf(output, capacity, "%s", exe_cores);
        return true;
    }
    if (storage_cores[0]) snprintf(output, capacity, "%s", storage_cores);
    return storage_cores[0] != '\0';
}

size_t bongo_cat_renderer_module_scan(BongoCatRendererModuleInfo *entries,
    size_t capacity) {
    cached_count = 0;
    char directory[BONGO_CAT_PATH_CAP];
    bool exists = false;
    if (!capacity || !bongo_cat_renderer_module_directory(directory,
            sizeof(directory), &exists)) {
        SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
            "[live2d] 无法解析渲染组件目录: Live2D 渲染不可用(其余功能不受影响)");
        return 0;
    }
    if (!exists) {
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[live2d] 渲染组件目录不存在: %s", directory);
        return 0;
    }
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE, "[live2d] 扫描渲染组件目录: %s",
        directory);
    ModuleScan scan = {entries, &cached_count, capacity};
    if (!bongo_cat_path_enumerate(directory, scan_visitor, &scan))
        SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE, "[live2d] 目录枚举失败: %s",
            directory);
    if (!cached_count)
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[live2d] 组件目录里没有可用的渲染组件; 应用照常启动, "
            "Live2D 渲染走诊断兜底");
    return cached_count;
}

size_t bongo_cat_renderer_module_last(BongoCatRendererModuleInfo **entries) {
    if (entries) *entries = cached;
    return cached_count;
}

bool bongo_cat_renderer_module_live2d_available(void) {
    for (size_t i = 0; i < cached_count; ++i) {
        if (cached[i].state != BONGO_CAT_RENDERER_MODULE_READY) continue;
        if (!strncmp(cached[i].name, BONGO_CAT_RENDERER_MODULE_LIVE2D,
                strlen(BONGO_CAT_RENDERER_MODULE_LIVE2D)))
            return true;
    }
    return false;
}
