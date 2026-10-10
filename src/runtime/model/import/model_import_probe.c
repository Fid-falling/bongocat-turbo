/*
 * 精确格式探测: 只认"传入目录自己就是某个格式的包根", 不做向上或向下搜索(那属于
 * discover)。返回值是三态 int: > 0 命中, 0 不匹配, < 0 命中但包损坏; STRICT 与 FALLBACK
 * 决定损坏时是否继续试后面的适配器。
 * live2d_owner 是从 .moc3 反查归属包的入口, 只在祖先目录里查找。
 */
#include "model_import_probe.h"
#include "model_import_path.h"
#include "mver/model_import_mver.h"
#include "tauri/model_import_tauri.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 适配器探测的统一签名: 返回值 > 0 命中(实际恒为 1), 0 不匹配, < 0 命中但包损坏;
   负值同时写入 BongoCatError, 因为它是用户可见的导入失败。 */
typedef int (*ExactProbe)(const char *source,
    BongoCatImportDiscovery *discovery, BongoCatError *error);

/* 先问 Mver(它会向上寻找父包, 并保护内置 Live2D 模型不被当成独立 Tauri 源转换),
   不命中再试 Tauri 精确探测。discovery 先清零, 错误由子适配器写入。 */
static int probe_live2d_owner_at(const char *directory,
    BongoCatImportDiscovery *discovery, BongoCatError *error) {
    memset(discovery, 0, sizeof(*discovery));
    /* Mver's adapter owns parent lookup and protects bundled Live2D models
       from being converted as standalone Tauri sources. */
    int found = bongo_cat_import_mver_discover(directory, discovery, error);
    return found ? found : bongo_cat_import_tauri_discover_exact(directory,
        discovery, error);
}

/* 从 .moc3 所在目录起最多上溯 12 层, 命中就输出归属包根路径
   (discovery->candidates[0].package_root)。只有成功写出 directory 才返回 OK; 命中但
   路径放不下返回 ARGUMENT; 一直找到根仍未命中报 FORMAT。 */
BongoCatResult bongo_cat_import_probe_live2d_owner(const char *source,
    char *directory, size_t capacity, BongoCatError *error) {
    char current[BONGO_CAT_PATH_CAP];
    if (!directory || !capacity ||
        !bongo_cat_import_parent_path(source, current, sizeof(current))) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_ARGUMENT,
            "Cannot determine the selected model file directory");
        return BONGO_CAT_ERROR_ARGUMENT;
    }
    BongoCatImportDiscovery *discovery = calloc(1, sizeof(*discovery));
    if (!discovery) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY,
            "Cannot allocate model source discovery workspace");
        return BONGO_CAT_ERROR_MEMORY;
    }
    BongoCatResult result = BONGO_CAT_ERROR_FORMAT;
    int found = 0;
    for (int depth = 0; depth < 12; ++depth) {
        BongoCatError probe_error = {0};
        found = probe_live2d_owner_at(current, discovery, &probe_error);
        if (found < 0) {
            if (error) *error = probe_error;
            result = probe_error.code ? probe_error.code : BONGO_CAT_ERROR_FORMAT;
            break;
        }
        if (found > 0) {
            int length = snprintf(directory, capacity, "%s",
                discovery->candidates[0].package_root);
            result = length >= 0 && (size_t)length < capacity
                ? BONGO_CAT_OK : BONGO_CAT_ERROR_ARGUMENT;
            if (result != BONGO_CAT_OK) bongo_cat_error_set(error, result,
                "Model import path is too long");
            break;
        }
        char parent[BONGO_CAT_PATH_CAP];
        if (!bongo_cat_import_parent_path(current, parent, sizeof(parent)) ||
            strcmp(current, parent) == 0) break;
        snprintf(current, sizeof(current), "%s", parent);
    }
    free(discovery);
    if (!found) bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
        "No supported model package found above Live2D .moc3 file: %s", source);
    return result;
}

/* 日志与诊断用的稳定短名: 可以出现在用户日志里, 但不要用它做逻辑判断。 */
const char *bongo_cat_import_format_name(BongoCatImportFormat format) {
    switch (format) {
    case BONGO_CAT_IMPORT_MVER: return "mver";
    case BONGO_CAT_IMPORT_MVER_PATCH: return "mver-patch";
    case BONGO_CAT_IMPORT_TAURI: return "tauri";
    default: return "unknown";
    }
}

/* 跑一个适配器并补一条诊断日志; 日志只在 diagnostic 打开时输出(冒烟与 CI),
   常规导入不产生噪音。 */
static int run_probe(const char *source, BongoCatImportDiscovery *discovery,
    BongoCatImportFormat format, ExactProbe exact, bool diagnostic,
    uint64_t started, BongoCatError *error) {
    int found = exact(source, discovery, error);
    if (found && diagnostic) SDL_Log(
        "[runtime] Model import exact probe completed: format=%s result=%d "
        "candidates=%llu elapsed_ms=%.1f path=%s",
        bongo_cat_import_format_name(format), found,
        (unsigned long long)discovery->count,
        (SDL_GetTicksNS() - started) / 1000000.0, source);
    return found;
}

/* 按固定顺序 MVER -> MVER_PATCH -> TAURI 尝试; 每个适配器之前清空 discovery 与 error,
   避免上一个适配器的残留。STRICT 下"命中但损坏"的负值立即返回, FALLBACK 下继续试后面
   的适配器 —— 这个顺序决定了谁有优先解释权。 */
int bongo_cat_import_probe_exact(const char *source,
    BongoCatImportDiscovery *discovery, BongoCatImportFormat *format,
    BongoCatImportProbePolicy policy, bool diagnostic,
    BongoCatError *error) {
    if (!source || !discovery) return 0;
    uint64_t started = SDL_GetTicksNS();
    static const struct {
        BongoCatImportFormat format;
        ExactProbe probe;
    } probes[] = {
        {BONGO_CAT_IMPORT_MVER, bongo_cat_import_mver_discover_exact},
        {BONGO_CAT_IMPORT_MVER_PATCH,
            bongo_cat_import_mver_patch_discover_exact},
        {BONGO_CAT_IMPORT_TAURI, bongo_cat_import_tauri_discover_exact}
    };
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); ++i) {
        memset(discovery, 0, sizeof(*discovery));
        if (error) *error = (BongoCatError){0};
        int found = run_probe(source, discovery, probes[i].format,
            probes[i].probe, diagnostic, started, error);
        if (found == 0) continue;
        if (found < 0 && policy == BONGO_CAT_IMPORT_PROBE_FALLBACK &&
            i + 1 < sizeof(probes) / sizeof(probes[0])) continue;
        if (format) *format = probes[i].format;
        return found;
    }
    if (diagnostic) SDL_Log(
        "[runtime] Model import exact probe completed: format=none result=0 "
        "candidates=0 elapsed_ms=%.1f path=%s",
        (SDL_GetTicksNS() - started) / 1000000.0, source);
    return 0;
}
