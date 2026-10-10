/*
 * 来源识别的总入口, 按优先级依次尝试: Mver 包(含向上寻找父包) -> Mver 贴图补丁 ->
 * Tauri 精确探测 -> 容器扫描(递归找包根) -> Tauri 递归扫描(最后兜底)。
 * 契约: 每次尝试前清零 discovery 并重新填好 source_name(用户选中的目录名会成为可见
 * 模型名); 返回 true 时 count > 0。与 model_import_probe.h 的分工是"广域识别"对
 * "精确包根分发"。
 */
#include "model_import.h"
#include "mver/model_import_mver.h"
#include "tauri/model_import_tauri.h"
#include "bongo_cat/path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 先去掉尾部斜杠再取 basename: 从文件管理器拖进来的路径常带分隔符, 直接取会得到空名字,
   进而丢掉可见模型名。路径超长时放弃填充而不是截断。 */
static void seed_source_name(const char *source,
    BongoCatImportDiscovery *discovery) {
    char path[BONGO_CAT_PATH_CAP];
    int written = snprintf(path, sizeof(path), "%s", source ? source : "");
    if (written < 0 || (size_t)written >= sizeof(path)) return;
    size_t length = strlen(path);
    while (length && (path[length - 1] == '/' || path[length - 1] == '\\'))
        path[--length] = '\0';
    const char *name = bongo_cat_path_name(path);
    if (name && name[0]) snprintf(discovery->source_name,
        sizeof(discovery->source_name), "%s", name);
}

/* 排序优先级 standard < keyboard < gamepad: receipt.ids[0] 会成为导入后选中的模型,
   让没有特殊需求的包默认落在标准模式。 */
static int rank(const BongoCatImportCandidate *candidate) {
    return candidate->mode == BONGO_CAT_MODE_STANDARD ? 0 :
        candidate->mode == BONGO_CAT_MODE_KEYBOARD ? 1 : 2;
}

/* 同一模式内优先原生 Mver(无需转换), 其次 Tauri, 贴图补丁排最后(它是增量, 需要基包)。 */
static int format_rank(const BongoCatImportCandidate *candidate) {
    return candidate->format == BONGO_CAT_IMPORT_MVER ? 0 :
        candidate->format == BONGO_CAT_IMPORT_TAURI ? 1 : 2;
}

/* qsort 比较函数: 前两级相同时用 directory 与 patch_root 全序化, 保证同一输入目录每次
   都得到相同顺序(去重与家族摘要都依赖顺序稳定)。 */
static int compare_candidates(const void *left, const void *right) {
    const BongoCatImportCandidate *a = left, *b = right;
    int difference = rank(a) - rank(b);
    if (difference) return difference;
    difference = format_rank(a) - format_rank(b);
    if (difference) return difference;
    difference = strcmp(a->directory, b->directory);
    return difference ? difference : strcmp(a->patch_root, b->patch_root);
}

typedef struct ContainerDiscovery {
    BongoCatImportDiscovery *output;
} ContainerDiscovery;

/* 扫描回调: 把子目录发现的候选追加进同一次导入。候选表满时直接报错而不是截断,
   因为截断会静默少导入几个模型变体。 */
static BongoCatResult collect_container(void *userdata, const char *source,
    BongoCatImportDiscovery *found, BongoCatError *error) {
    (void)source;
    ContainerDiscovery *container = userdata;
    for (size_t i = 0; i < found->count; ++i) {
        if (container->output->count >= BONGO_CAT_IMPORT_CANDIDATE_CAP) {
            bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
                "Selected folder contains too many model variants");
            return BONGO_CAT_ERROR_FORMAT;
        }
        container->output->candidates[container->output->count++] =
            found->candidates[i];
    }
    return BONGO_CAT_OK;
}

/* 失败传播规则: 子适配器返回负值表示"命中但包损坏", 此时不再降级到后面的格式(用户确实
   选中了一个坏包); 贴图补丁的错误先存起来, 只有后续全部落空时才上报, 否则它可能掩盖
   真正命中的格式。候选收集完按 compare_candidates 排序后返回。 */
bool bongo_cat_import_discover(const char *source,
    BongoCatImportDiscovery *discovery, BongoCatError *error) {
    memset(discovery, 0, sizeof(*discovery));
    seed_source_name(source, discovery);
    /* Broad package discovery intentionally stays separate from exact probe
       dispatch: Mver may be selected from one of its authored subfolders,
       while Tauri conversion starts only from an owning package root. */
    int mver = bongo_cat_import_mver_discover(source, discovery, error);
    if (mver > 0) return true;
    if (mver < 0) return false;
    BongoCatError patch_error = {0};
    int patch = bongo_cat_import_mver_patch_discover(source, discovery,
        &patch_error);
    if (patch > 0) return true;
    memset(discovery, 0, sizeof(*discovery));
    seed_source_name(source, discovery);
    int tauri = bongo_cat_import_tauri_discover_exact(source, discovery, error);
    if (tauri > 0) return true;
    if (tauri < 0) return false;
    memset(discovery, 0, sizeof(*discovery));
    seed_source_name(source, discovery);
    ContainerDiscovery container = {discovery};
    BongoCatResult result = bongo_cat_import_scan(source, collect_container,
        &container, error);
    if (result != BONGO_CAT_OK) return false;
    if (discovery->count) {
        /* Recursive discovery returns the owning package root on each
           candidate. Re-rooting Mver candidates at the selected container
           would copy unrelated sibling folders and break normalization. */
        qsort(discovery->candidates, discovery->count,
            sizeof(discovery->candidates[0]), compare_candidates);
        return true;
    }
    if (patch < 0) {
        if (error) *error = patch_error;
        return false;
    }
    if (error) *error = (BongoCatError){0};
    int recursive = bongo_cat_import_tauri_discover_recursive(source,
        discovery, error);
    if (recursive < 0) return false;
    if (!recursive) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            "Selected directory contains no valid Live2D model3 JSON");
        return false;
    }
    qsort(discovery->candidates, discovery->count,
        sizeof(discovery->candidates[0]), compare_candidates);
    return true;
}
