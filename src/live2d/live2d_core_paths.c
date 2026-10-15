/*
 * Cubism Core 的位置解析与运行期装载(宿主侧)。
 *
 * 命名约定: 目录统一叫 `cores`, 不叫 live2d-core/plugins —— 我们不兼容任何第三方插件 ABI,
 * 也没有插件生态。要区分的是**归属**:
 *   <存储根>/cores/<平台>/<架构>/<库名>  ← 用户自备的专有 Core(本文件要找的东西)
 *   <exe 目录>/cores/bongo_live2d.*      ← 我们编译的渲染组件(见 renderer_module.h)
 *
 * 关键不变式:
 *   1. **只有宿主加载 Core**。组件从不打开 Core 文件, 它通过宿主服务表的 core_symbol 拿符号。
 *      理由: (a) "组件是否需要 Core" 要在选组件之前就知道(能力位协商);
 *          (b) 版本校验、SHA-256 显示、导入/回滚与 UI 提示都必须由宿主统一负责。
 *   2. 找不到 Core 不是错误: 程序照常启动, Live2D 走 live2d_stub.c 的兜底实现。
 *   3. 已加载的句柄一直持有到"实例与 GPU 资源都销毁"之后, 否则替换 Core 时会有悬空函数指针。
 *
 * 查找顺序(见 LIVE2D-PLUGIN-PLAN.md §9.2 与 §P0.8 的命名修正):
 *   $BONGO_CAT_LIVE2D_CORE → <存储根>/cores/<平台>/<架构>/ → <存储根>/cores/
 *   → <exe 目录>/cores/<平台>/<架构>/ → <exe 目录>/cores/
 * 每一级都要通过三符号探测才算命中: 目录里放着一个同名但残缺的 DLL 时, 我们要继续往下找,
 * 而不是拿它去崩。
 */
#include "bongo_cat/live2d_core.h"
#include "bongo_cat/log.h"
#include "bongo_cat/path.h"

#include "storage_paths.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <string.h>

/* 一次最多检查多少个候选路径: 5 个位置 × (裸文件 + 平台/架构目录) 足够, 留余量。 */
#define CORE_CANDIDATE_LIMIT 12

typedef unsigned int (*CoreGetVersionFn)(void);

static void *core_handle;
static char core_path[BONGO_CAT_PATH_CAP];
static uint32_t core_version;

const char *bongo_cat_live2d_core_platform_name(void) {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

const char *bongo_cat_live2d_core_arch_name(void) {
#if defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#elif defined(_WIN64) || defined(__x86_64__)
    return "x86_64";
#else
    return "x86";
#endif
}

const char *bongo_cat_live2d_core_library_name(void) {
#if defined(_WIN32)
    return "Live2DCubismCore.dll";
#elif defined(__APPLE__)
    return "libLive2DCubismCore.dylib";
#else
    return "libLive2DCubismCore.so";
#endif
}

/* 存储根未解析时(启动极早期或存储不可用)退到 <exe 目录>: 用户"把 Core 丢在 exe 旁边"
   这条路必须一直可用, 否则存储目录出问题时连补救办法都没有。 */
static bool base_root(char *output, size_t capacity, const char *child) {
    char root[BONGO_CAT_PATH_CAP];
    if (bongo_cat_storage_paths_root(root, sizeof(root)))
        return bongo_cat_path_join(output, capacity, root, child);
    const char *base = SDL_GetBasePath();
    /* SDL_GetBasePath 返回的是 SDL 自有内存, 不能 free(踩坑见 CHANGELOG: 重复 free 会堆损坏)。 */
    return base && bongo_cat_path_join(output, capacity, base, child);
}

bool bongo_cat_live2d_core_root_directory(char *output, size_t capacity) {
    return base_root(output, capacity, "cores");
}

bool bongo_cat_live2d_core_install_directory(char *output, size_t capacity) {
    char root[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_live2d_core_root_directory(root, sizeof(root))) return false;
    char platform[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(platform, sizeof(platform), root,
            bongo_cat_live2d_core_platform_name())) return false;
    return bongo_cat_path_join(output, capacity, platform,
        bongo_cat_live2d_core_arch_name());
}

bool bongo_cat_live2d_core_staging_directory(char *output, size_t capacity) {
    char root[BONGO_CAT_PATH_CAP];
    return bongo_cat_live2d_core_root_directory(root, sizeof(root)) &&
        bongo_cat_path_join(output, capacity, root, ".staging");
}

/* 往候选表里追加: 目录形式(<dir>/<库名>)与裸文件都接受, 重复路径只留一份, 免得同一个
   文件被探测多次、日志里出现两遍相同结论。 */
static void add_candidate(char paths[][BONGO_CAT_PATH_CAP], size_t *count,
    const char *candidate) {
    if (!candidate || !candidate[0] || *count >= CORE_CANDIDATE_LIMIT) return;
    for (size_t i = 0; i < *count; ++i)
        if (!strcmp(paths[i], candidate)) return;
    snprintf(paths[*count], BONGO_CAT_PATH_CAP, "%s", candidate);
    ++(*count);
}

/* $BONGO_CAT_LIVE2D_CORE 指向目录时补上库名, 指向文件时原样使用。 */
static void add_env_candidate(char paths[][BONGO_CAT_PATH_CAP], size_t *count) {
    const char *configured = SDL_getenv("BONGO_CAT_LIVE2D_CORE");
    if (!configured || !configured[0]) return;
    if (bongo_cat_path_is_dir(configured)) {
        char joined[BONGO_CAT_PATH_CAP];
        if (bongo_cat_path_join(joined, sizeof(joined), configured,
                bongo_cat_live2d_core_library_name()))
            add_candidate(paths, count, joined);
        return;
    }
    add_candidate(paths, count, configured);
}

static size_t collect_candidates(char paths[][BONGO_CAT_PATH_CAP]) {
    size_t count = 0;
    const char *library = bongo_cat_live2d_core_library_name();
    add_env_candidate(paths, &count);

    char root[BONGO_CAT_PATH_CAP], install[BONGO_CAT_PATH_CAP];
    if (bongo_cat_live2d_core_root_directory(root, sizeof(root))) {
        char joined[BONGO_CAT_PATH_CAP];
        if (bongo_cat_live2d_core_install_directory(install, sizeof(install)) &&
            bongo_cat_path_join(joined, sizeof(joined), install, library))
            add_candidate(paths, &count, joined);
        if (bongo_cat_path_join(joined, sizeof(joined), root, library))
            add_candidate(paths, &count, joined);
    }

    /* <exe 目录>/cores/**: 便携/绿色用法下最自然的投放点(与我们的渲染组件同目录,
       但按 <平台>/<架构> 分子目录, 所以两者不会混在一起)。 */
    const char *base = SDL_GetBasePath();
    if (base) {
        char exe_root[BONGO_CAT_PATH_CAP], joined[BONGO_CAT_PATH_CAP];
        char platform[BONGO_CAT_PATH_CAP];
        if (bongo_cat_path_join(exe_root, sizeof(exe_root), base, "cores")) {
            if (bongo_cat_path_join(platform, sizeof(platform), exe_root,
                    bongo_cat_live2d_core_platform_name()) &&
                bongo_cat_path_join(joined, sizeof(joined), platform,
                    bongo_cat_live2d_core_arch_name())) {
                char file[BONGO_CAT_PATH_CAP];
                if (bongo_cat_path_join(file, sizeof(file), joined, library))
                    add_candidate(paths, &count, file);
            }
            if (bongo_cat_path_join(joined, sizeof(joined), exe_root, library))
                add_candidate(paths, &count, joined);
        }
    }
    return count;
}

uint32_t bongo_cat_live2d_core_version(void) { return core_version; }

const char *bongo_cat_live2d_core_loaded_path(void) { return core_path; }

bool bongo_cat_live2d_core_available(void) { return core_handle != NULL; }

void *bongo_cat_live2d_core_symbol(const char *name) {
    return core_handle ? bongo_cat_platform_live2d_core_symbol(core_handle, name)
        : NULL;
}

void bongo_cat_live2d_core_shutdown(void) {
    if (!core_handle) return;
    bongo_cat_platform_live2d_core_close(core_handle);
    core_handle = NULL;
    core_version = 0;
    core_path[0] = '\0';
}

bool bongo_cat_live2d_core_initialize(void) {
    if (core_handle) return true;
    char paths[CORE_CANDIDATE_LIMIT][BONGO_CAT_PATH_CAP];
    size_t count = collect_candidates(paths);
    const char *library = bongo_cat_live2d_core_library_name();
    for (size_t i = 0; i < count; ++i) {
        if (!bongo_cat_path_is_file(paths[i])) continue;
        if (!bongo_cat_platform_live2d_core_arch_matches(paths[i])) {
            SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
                "[live2d] 架构不匹配的 Core 被跳过(本机 %s/%s): %s",
                bongo_cat_live2d_core_platform_name(),
                bongo_cat_live2d_core_arch_name(), paths[i]);
            continue;
        }
        BongoCatError error = {0};
        void *handle = bongo_cat_platform_live2d_core_open(paths[i], &error);
        if (!handle) {
            SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE, "[live2d] %s",
                error.message[0] ? error.message : "Cubism Core 加载失败");
            continue;
        }
        if (!bongo_cat_platform_live2d_core_probe(handle)) {
            bongo_cat_platform_live2d_core_close(handle);
            continue;
        }
        CoreGetVersionFn version_fn = (CoreGetVersionFn)
            bongo_cat_platform_live2d_core_symbol(handle, "csmGetVersion");
        core_handle = handle;
        core_version = version_fn ? version_fn() : 0;
        snprintf(core_path, sizeof(core_path), "%s", paths[i]);
        /* 版本号是整数: 早期把它当字符串指针传给 %s 会崩在 printf 内部(CHANGELOG 有记录)。 */
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[live2d] 已加载用户提供的 Cubism Core (%s, csm 版本 %u): %s", library,
            (unsigned)core_version, core_path);
        return true;
    }
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[live2d] 未找到可用的 Cubism Core (%s): Live2D 渲染暂时不可用, "
        "其余功能不受影响; 在设置里导入官方 SDK 压缩包或 Core 文件即可启用",
        library);
    return false;
}
