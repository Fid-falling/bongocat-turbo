/*
 * 渐进式批量导入: 一个来源(选中的目录或 zip) 展开成多个可安装单元逐个安装, 单个单元失败不中断
 * 整批, 所以"部分成功"是正常结果。时序: 导入线程 -> 本文件 -> model_import_scan.c 的限时容器
 * 扫描 -> install_one -> model_import_install.c 的事务提交, 存储锁由 session_install 内部获取。
 * 契约: stats 按来源单元计数而不是变体数, 内容完全重复的包也算成功; 返回码只是第一个失败单元的
 * 错误, 此前成功的单元已经落盘, 调用方不能据此回滚。
 * 最容易改错: patch_only 的冗余判断只比对本次扫描发现的完整包, 不查 models_root。
 */
#include "model_import.h"
#include "model_import_archive.h"
#include "model_import_probe.h"
#include "bongo_cat/path.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ProgressiveBase {
    char directory[BONGO_CAT_PATH_CAP];
    char setting[BONGO_CAT_PATH_CAP];
    BongoCatModelMode mode;
} ProgressiveBase;

typedef struct ProgressiveUnit {
    char source[BONGO_CAT_PATH_CAP];
    bool patch_only;
} ProgressiveUnit;

/* 扫描工作区: units 与 bases 各 128 项, 每项含一到两条 BONGO_CAT_PATH_CAP 路径, 合计数百 KB,
 * 只能堆分配; 生命周期限于一次 install_progressive_directory 调用。 */
typedef struct ProgressiveScan {
    ProgressiveUnit units[BONGO_CAT_MODEL_CAP];
    ProgressiveBase bases[BONGO_CAT_MODEL_CAP];
    size_t unit_count;
    size_t base_count;
    size_t model_count;
} ProgressiveScan;

/* 容器扫描的访问回调: 每个被识别为包根的目录登记成一个安装单元, 其中的完整 Mver 候选同时记进
 * bases, 供后面的补丁冗余判断使用。
 * patch_only 表示该单元清一色是补丁 —— 这种单元可能只是给旁边完整包打的补丁, 单独安装会重复。
 * 返回非 OK 不会让遍历停下(扫描只记住第一个错误), 但整次扫描最终会以该错误收场。 */
static BongoCatResult collect_unit(void *userdata, const char *source,
    BongoCatImportDiscovery *discovery, BongoCatError *error) {
    ProgressiveScan *scan = userdata;
    if (scan->unit_count >= BONGO_CAT_MODEL_CAP ||
        discovery->count > BONGO_CAT_MODEL_CAP - scan->model_count) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            "Selected folder contains too many model variants");
        return BONGO_CAT_ERROR_FORMAT;
    }
    ProgressiveUnit *unit = &scan->units[scan->unit_count++];
    snprintf(unit->source, sizeof(unit->source), "%s", source);
    unit->patch_only = discovery->count > 0;
    scan->model_count += discovery->count;
    for (size_t i = 0; i < discovery->count; ++i) {
        const BongoCatImportCandidate *candidate = &discovery->candidates[i];
        if (candidate->format != BONGO_CAT_IMPORT_MVER_PATCH)
            unit->patch_only = false;
        if (candidate->format != BONGO_CAT_IMPORT_MVER) continue;
        if (scan->base_count >= BONGO_CAT_MODEL_CAP) {
            bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
                "Selected folder contains too many model variants");
            return BONGO_CAT_ERROR_FORMAT;
        }
        ProgressiveBase *base = &scan->bases[scan->base_count++];
        snprintf(base->directory, sizeof(base->directory), "%s",
            candidate->directory);
        snprintf(base->setting, sizeof(base->setting), "%s",
            candidate->setting);
        base->mode = candidate->mode;
    }
    return BONGO_CAT_OK;
}

/* 本次扫描里是否已有该补丁依附的完整包: 模式、清单目录与清单文件名三者全同才算命中, 判据刻意
 * 与 bongo_cat_import_patch_has_full_base 保持一致, 改一处就要改两处。 */
static bool has_base(const ProgressiveScan *scan,
    const BongoCatImportCandidate *patch) {
    for (size_t i = 0; i < scan->base_count; ++i) {
        const ProgressiveBase *base = &scan->bases[i];
        if (base->mode == patch->mode &&
            strcmp(base->directory, patch->directory) == 0 &&
            strcmp(base->setting, patch->setting) == 0) return true;
    }
    return false;
}

/* 补丁单元是否已被本次扫描中的完整包覆盖。补丁单元要额外做一次精确探测(STRICT 策略)才能拿到
 * 候选清单, 代价是重复一次 probe_exact; 探测失败时返回 false 保守放行, 让单元照常安装并在那里
 * 报出真正的错误 —— 这里的失败只该影响去重, 不该升级成整批导入失败。 */
static bool redundant_patch(const ProgressiveScan *scan,
    const ProgressiveUnit *unit) {
    if (!unit->patch_only) return false;
    BongoCatImportDiscovery *discovery = calloc(1, sizeof(*discovery));
    if (!discovery) return false;
    BongoCatError ignored = {0};
    bool redundant = bongo_cat_import_probe_exact(unit->source, discovery,
        NULL, BONGO_CAT_IMPORT_PROBE_STRICT, false, &ignored) > 0 &&
        discovery->count > 0;
    for (size_t i = 0; redundant && i < discovery->count; ++i)
        if (!has_base(scan, &discovery->candidates[i])) redundant = false;
    free(discovery);
    return redundant;
}

/* 记下失败来源的目录名供界面提示: 名额只有 BONGO_CAT_IMPORT_FAILURE_NAME_CAP 条, 满了就静默
 * 丢弃, 细节仍在日志与 BongoCatError 里。 */
static void record_failure_name(BongoCatImportBatchStats *stats,
    const char *source) {
    if (!stats || stats->failure_name_count >=
        BONGO_CAT_IMPORT_FAILURE_NAME_CAP) return;
    const char *name = bongo_cat_path_name(source);
    if (!name || !name[0]) return;
    snprintf(stats->failure_names[stats->failure_name_count++],
        BONGO_CAT_ID_CAP, "%s", name);
}

/* 安装单个单元并累计统计: 只有成功才回调 callback 交付 receipt; receipt 是栈上临时量, 回调期间
 * 借用, 回调返回后失效。error 不由本函数写, 由调用方决定是否保存。 */
static BongoCatResult install_one(BongoCatImportSession *session,
    const char *source, BongoCatImportReceiptCallback callback,
    void *userdata, BongoCatImportBatchStats *stats, BongoCatError *error) {
    BongoCatImportReceipt receipt = {0};
    uint64_t started = SDL_GetTicksNS();
    SDL_Log("[runtime] Model import unit started: path=%s", source);
    BongoCatResult result = bongo_cat_import_session_install(session, source,
        &receipt, error);
    if (result == BONGO_CAT_OK) stats->succeeded_count++;
    else {
        stats->failed_count++;
        record_failure_name(stats, source);
    }
    if (result == BONGO_CAT_OK && callback)
        callback(userdata, &receipt);
    SDL_Log("[runtime] Model import unit completed: result=%d variants=%llu "
        "installed=%llu elapsed_ms=%.1f path=%s", (int)result,
        (unsigned long long)receipt.count,
        (unsigned long long)receipt.installed_count,
        (SDL_GetTicksNS() - started) / 1000000.0, source);
    return result;
}

/* 统一的失败登记: 记一次失败统计与来源名, 原样返回错误码, 便于 return fail_source(...) 收尾。 */
static BongoCatResult fail_source(BongoCatImportBatchStats *stats,
    const char *source, BongoCatResult result) {
    stats->failed_count++;
    record_failure_name(stats, source);
    return result;
}

/* 目录来源的渐进导入主流程。前提: stats 非空, 进来先清零, 保证失败时调用方读到的也是 0。
 * 步骤: 归一化来源目录 -> 精确探测走单包快路径 -> 否则限时容器扫描 -> 逐个单元安装; 扫描一个
 * 单元都没找到时退回"整包安装", 好让损坏包的报错落在用户真正选中的目录上。
 * 副作用: 成功的单元留在 models_root 且失败单元不回滚, 只把第一个失败的 error 带出去。 */
static BongoCatResult install_progressive_directory(
    BongoCatImportSession *session, const char *source,
    BongoCatImportReceiptCallback callback, void *userdata,
    BongoCatImportBatchStats *stats, BongoCatError *error) {
    if (!stats) return BONGO_CAT_ERROR_ARGUMENT;
    *stats = (BongoCatImportBatchStats){0};
    if (!session || !source)
        return fail_source(stats, source, BONGO_CAT_ERROR_ARGUMENT);
    char directory[BONGO_CAT_PATH_CAP];
    BongoCatResult normalized = bongo_cat_import_source_directory(source,
        directory, sizeof(directory), error);
    if (normalized != BONGO_CAT_OK)
        return fail_source(stats, source, normalized);

    SDL_Log("[runtime] Model import progressive discovery started: path=%s",
        directory);
    BongoCatImportDiscovery *exact = calloc(1, sizeof(*exact));
    if (!exact) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY,
            "Cannot allocate exact model discovery workspace");
        return fail_source(stats, directory, BONGO_CAT_ERROR_MEMORY);
    }
    BongoCatError exact_error = {0};
    int exact_result = bongo_cat_import_probe_exact(directory, exact, NULL,
        BONGO_CAT_IMPORT_PROBE_STRICT, true, &exact_error);
    free(exact);
    /* 精确命中: 选中的就是包根, 不再往下扫, 免得把同一个包里的子目录重复装一遍。 */
    if (exact_result > 0) {
        SDL_Log("[runtime] Model import progressive discovery selected exact "
            "package: path=%s", directory);
        return install_one(session, directory, callback, userdata, stats,
            error);
    }
    /* STRICT 策略下"命中但损坏"是真错误: 不降级去扫子目录, 否则用户只会看到"没找到模型"。 */
    if (exact_result < 0) {
        if (error) *error = exact_error;
        return fail_source(stats, directory, exact_error.code
            ? exact_error.code : BONGO_CAT_ERROR_FORMAT);
    }

    ProgressiveScan *scan = calloc(1, sizeof(*scan));
    if (!scan) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY,
            "Cannot allocate progressive model import scan");
        return fail_source(stats, directory, BONGO_CAT_ERROR_MEMORY);
    }
    uint64_t scan_started = SDL_GetTicksNS();
    SDL_Log("[runtime] Model import container scan started: path=%s",
        directory);
    BongoCatResult scanned = bongo_cat_import_scan_diagnostic(directory,
        collect_unit, scan, error);
    SDL_Log("[runtime] Model import container scan completed: result=%d "
        "units=%llu variants=%llu elapsed_ms=%.1f path=%s", (int)scanned,
        (unsigned long long)scan->unit_count,
        (unsigned long long)scan->model_count,
        (SDL_GetTicksNS() - scan_started) / 1000000.0, directory);
    /* 扫描没找到可导入单元(或扫描本身失败): 把选中目录当整包再装一次, 报错才会指向用户真正
       选中的那个目录。 */
    if (scanned != BONGO_CAT_OK || !scan->unit_count) {
        free(scan);
        return scanned != BONGO_CAT_OK ? fail_source(stats, directory, scanned) :
            install_one(session, directory, callback, userdata, stats, error);
    }

    BongoCatResult first_failure = BONGO_CAT_OK;
    BongoCatError first_error = {0};
    /* 单元之间互不影响, 只记第一个失败即可; 各失败单元的错误已经写进 install_one 的日志。 */
    for (size_t i = 0; i < scan->unit_count; ++i) {
        if (redundant_patch(scan, &scan->units[i])) continue;
        BongoCatError local = {0};
        BongoCatResult result = install_one(session, scan->units[i].source,
            callback, userdata, stats, &local);
        if (result != BONGO_CAT_OK && first_failure == BONGO_CAT_OK) {
            first_failure = result;
            first_error = local;
        }
    }
    free(scan);
    if (first_failure != BONGO_CAT_OK && error) *error = first_error;
    return first_failure;
}

/* 批量导入入口: zip 来源先解压到临时目录, 安装返回后立刻清理解压树 —— 需要的内容已经复制进
 * models_root, 此后不再引用临时目录。session/source 为 NULL 或来源不可导入时都落到目录分支,
 * 由那里记一次失败统计, 所以 stats 在任何路径上都会被清零。 */
BongoCatResult bongo_cat_import_session_install_progressive(
    BongoCatImportSession *session, const char *source,
    BongoCatImportReceiptCallback callback, void *userdata,
    BongoCatImportBatchStats *stats, BongoCatError *error) {
    if (!stats) return BONGO_CAT_ERROR_ARGUMENT;
    if (!session || !source || !bongo_cat_import_is_archive(source))
        return install_progressive_directory(session, source, callback,
            userdata, stats, error);
    *stats = (BongoCatImportBatchStats){0};
    char directory[BONGO_CAT_PATH_CAP], temporary[BONGO_CAT_PATH_CAP];
    BongoCatResult result = bongo_cat_import_archive_extract(source,
        directory, temporary, error);
    if (result != BONGO_CAT_OK) return fail_source(stats, source, result);
    result = install_progressive_directory(session, directory, callback,
        userdata, stats, error);
    bongo_cat_import_archive_cleanup(temporary);
    return result;
}
