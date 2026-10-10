/*
 * 安装事务: 在 models_root 下用 .import-<摘要前 16 位>-1.tmp 组装 -> 复检归一化结果 ->
 * 内容去重 -> 改名提交 -> 登记会话内的包索引。临时名必须符合 model_storage.c 里
 * temporary_name 的格式(.import-十六进制-数字.tmp), 否则启动清理认不出来。
 * 幂等: 内容摘要与模式集合都相同的包再次导入返回 OK, 但 receipt->installed_count 为 0。
 * 前提: 调用方必须已持有存储锁(session_install 持锁后才调到这里), 本文件不再加锁。
 * 最容易改错: 每条失败路径都要清理临时目录, 漏一条就留下垃圾直到下次会话清理。
 */
#include "model_import_session_internal.h"
#include "model_storage.h"
#include "bongo_cat/path.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct ImportInstall {
    char id[BONGO_CAT_ID_CAP];
    char temporary[BONGO_CAT_PATH_CAP];
    char target[BONGO_CAT_PATH_CAP];
    bool committed;
} ImportInstall;

/* 回滚: 未提交时删临时目录, 已提交时删目标目录 —— 后者用于"改名成功但索引登记失败"
   这种极端情况。error 传 NULL, 清理失败只影响磁盘残留, 不改变调用方的返回码。 */
static void cleanup(ImportInstall *install) {
    if (!install) return;
    bongo_cat_model_remove_tree(install->committed
        ? install->target : install->temporary, NULL);
}

/* packages 索引是"本次会话开始时 models_root 里已有什么"的快照, 安装成功后会被追加;
   内容去重只在这个快照与本次已提交的包之间进行。 */
static bool existing_package(BongoCatImportSession *session,
    const BongoCatImportDiscovery *discovery,
    const BongoCatPackageMetadata *metadata,
    char ids[BONGO_CAT_IMPORT_CANDIDATE_CAP][BONGO_CAT_ID_CAP],
    size_t *count) {
    return bongo_cat_import_package_index_find(session->packages,
        discovery, metadata, ids, count);
}

/* 只有全部候选都是原生 Mver 时才能提前去重: 那时源包内容与最终存储内容一致, 摘要可直接
   比较。Tauri 要转换、贴图补丁要合并基包, 都得等归一化之后才知道最终摘要。 */
static bool canonical_mver_package(
    const BongoCatImportDiscovery *discovery) {
    if (!discovery || !discovery->count) return false;
    for (size_t i = 0; i < discovery->count; ++i)
        if (discovery->candidates[i].format != BONGO_CAT_IMPORT_MVER)
            return false;
    return true;
}

/* 从 base 开始依次试 base、base-2、base-3……, 取第一个在 models_root 下既不占目录也不
   占文件的 id(同名文件同样会让建目录失败)。上限 1 万次后报 FORMAT 错误, 不是无限循环。 */
static bool resolve_install_id(const char *base, const char *root,
    char output[BONGO_CAT_ID_CAP], BongoCatError *error) {
    for (size_t suffix = 1; suffix < 10000; ++suffix) {
        char id[BONGO_CAT_ID_CAP];
        int written = suffix == 1
            ? snprintf(id, sizeof(id), "%s", base)
            : snprintf(id, sizeof(id), "%s-%zu", base, suffix);
        if (written < 0 || (size_t)written >= sizeof(id)) continue;
        char path[BONGO_CAT_PATH_CAP];
        bool exists = bongo_cat_path_join(path, sizeof(path), root, id) &&
            (bongo_cat_path_is_dir(path) || bongo_cat_path_is_file(path));
        if (!exists) {
            snprintf(output, BONGO_CAT_ID_CAP, "%s", id);
            return true;
        }
    }
    bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
        "Cannot allocate a unique model directory for %s", base);
    return false;
}

/* 按包 id 加变体下标生成每个变体 id(见 bongo_cat_import_variant_id); installed 逐项
   写入, 只有真正提交成功才是 true。 */
static void fill_receipt(BongoCatImportReceipt *receipt, const char *id,
    size_t count, bool installed) {
    if (!receipt) return;
    receipt->count = count;
    receipt->installed_count = installed ? count : 0;
    for (size_t i = 0; i < count; ++i) {
        bongo_cat_import_variant_id(receipt->ids[i],
            sizeof(receipt->ids[i]), id, i);
        receipt->installed[i] = installed;
    }
}

/* 去重命中路径专用: 直接回填索引里已有的 id, installed 全为 false, 调用方据此知道
   本次没有新目录产生。 */
static void fill_receipt_ids(BongoCatImportReceipt *receipt,
    const char ids[BONGO_CAT_IMPORT_CANDIDATE_CAP][BONGO_CAT_ID_CAP],
    size_t count, bool installed) {
    if (!receipt) return;
    receipt->count = count;
    receipt->installed_count = installed ? count : 0;
    for (size_t i = 0; i < count; ++i) {
        snprintf(receipt->ids[i], sizeof(receipt->ids[i]), "%s", ids[i]);
        receipt->installed[i] = installed;
    }
}

/* 前提: session 有效且调用方持存储锁; source_metadata[i] 必须已与
   source_discovery->candidates[i] 对齐(即跑过 prepare_package_metadata*)。
   返回 OK 不代表"新装", 新增数量看 receipt->installed_count(receipt 可以为 NULL)。
   失败路径统一清理临时目录与堆内存; 本函数内唯一会删掉已提交目录的情况是索引登记失败。 */
BongoCatResult bongo_cat_import_install_discovery(
    BongoCatImportSession *session,
    BongoCatImportDiscovery *source_discovery,
    BongoCatPackageMetadata *source_metadata,
    BongoCatImportReceipt *receipt, BongoCatError *error) {
    uint64_t started = SDL_GetTicksNS();
    if (canonical_mver_package(source_discovery)) {
        char existing[BONGO_CAT_IMPORT_CANDIDATE_CAP][BONGO_CAT_ID_CAP];
        size_t existing_count = 0;
        if (existing_package(session, source_discovery, source_metadata,
                existing, &existing_count)) {
            SDL_Log("[runtime] Model import storage transaction: "
                "stage=source-duplicate variants=%llu elapsed_ms=%.1f",
                (unsigned long long)existing_count,
                (SDL_GetTicksNS() - started) / 1000000.0);
            fill_receipt_ids(receipt, existing, existing_count, false);
            return BONGO_CAT_OK;
        }
    }

    ImportInstall install = {0};
    char temporary_name[BONGO_CAT_ID_CAP + 24];
    snprintf(temporary_name, sizeof(temporary_name), ".import-%.16s-1.tmp",
        source_metadata[0].content_digest);
    if (!bongo_cat_path_join(install.temporary, sizeof(install.temporary),
            session->root, temporary_name)) return BONGO_CAT_ERROR_IO;
    SDL_Log("[runtime] Model import storage transaction: stage=prepare "
        "candidates=%llu temporary=%s",
        (unsigned long long)source_discovery->count, install.temporary);
    /* 先删同名残留(上次失败或崩溃留下的): 否则归一化会写进一个内容混杂的旧目录。 */
    bongo_cat_model_remove_tree(install.temporary, NULL);
    if (!bongo_cat_import_prepare_storage(source_discovery,
            install.temporary, error)) {
        cleanup(&install);
        return error && error->code == BONGO_CAT_ERROR_FORMAT
            ? BONGO_CAT_ERROR_FORMAT : BONGO_CAT_ERROR_IO;
    }
    SDL_Log("[runtime] Model import storage transaction: stage=prepared "
        "elapsed_ms=%.1f temporary=%s",
        (SDL_GetTicksNS() - started) / 1000000.0, install.temporary);

    /* discovery 结构体超过 110 KB(16 个候选 × 7 条路径 × 1024), 放堆上避免顶爆线程栈。 */
    BongoCatImportDiscovery *normalized = calloc(1, sizeof(*normalized));
    BongoCatPackageMetadata *metadata = calloc(BONGO_CAT_IMPORT_CANDIDATE_CAP,
        sizeof(*metadata));
    if (!normalized || !metadata) {
        free(normalized);
        free(metadata);
        cleanup(&install);
        bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY,
            "Cannot validate normalized model package");
        return BONGO_CAT_ERROR_MEMORY;
    }
    SDL_Log("[runtime] Model import storage transaction: "
        "stage=validate-normalized temporary=%s", install.temporary);
    bool discovered = bongo_cat_import_discover(install.temporary, normalized,
        error);
    SDL_Log("[runtime] Model import storage transaction: "
        "stage=normalized-discovered result=%d candidates=%llu temporary=%s",
        discovered, (unsigned long long)normalized->count, install.temporary);
    bool valid = discovered && bongo_cat_import_prepare_package_metadata_cached(
        normalized, metadata, session->digests, error);
    if (!valid) {
        free(normalized);
        free(metadata);
        cleanup(&install);
        return error && error->code ? error->code : BONGO_CAT_ERROR_FORMAT;
    }

    char existing[BONGO_CAT_IMPORT_CANDIDATE_CAP][BONGO_CAT_ID_CAP];
    size_t existing_count = 0;
    if (existing_package(session, normalized, metadata, existing,
            &existing_count)) {
        SDL_Log("[runtime] Model import storage transaction: stage=duplicate "
            "variants=%llu elapsed_ms=%.1f temporary=%s",
            (unsigned long long)existing_count,
            (SDL_GetTicksNS() - started) / 1000000.0, install.temporary);
        fill_receipt_ids(receipt, existing, existing_count, false);
        free(normalized);
        free(metadata);
        cleanup(&install);
        return BONGO_CAT_OK;
    }
    /* 用归一化后的变体数复查容量: 归一化会剔除重复变体与内置模板, 数量可能与源候选不同;
       放不下就整体拒绝, 而不是丢掉落不下的部分。 */
    if (!bongo_cat_import_package_index_has_capacity(session->packages,
            normalized->count)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            "Too many installed model variants");
        free(normalized);
        free(metadata);
        cleanup(&install);
        return BONGO_CAT_ERROR_FORMAT;
    }
    if (!resolve_install_id(source_metadata[0].package_id, session->root,
            install.id, error) ||
        !bongo_cat_path_join(install.target, sizeof(install.target),
            session->root, install.id)) {
        free(normalized);
        free(metadata);
        cleanup(&install);
        return error && error->code ? error->code : BONGO_CAT_ERROR_FORMAT;
    }
    /* 提交点: 同分区改名, Windows 走 MoveFileEx(REPLACE_EXISTING | WRITE_THROUGH),
       要么完整生效要么完全不发生, 因此 models_root 里不会出现半个包。 */
    if (!bongo_cat_path_rename(install.temporary, install.target)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Cannot finish model import: %s", SDL_GetError());
        free(normalized);
        free(metadata);
        cleanup(&install);
        return BONGO_CAT_ERROR_IO;
    }
    install.committed = true;
    SDL_Log("[runtime] Model import storage transaction: stage=committed "
        "id=%s elapsed_ms=%.1f target=%s", install.id,
        (SDL_GetTicksNS() - started) / 1000000.0, install.target);
    /* 目录已经在盘上但索引登记失败: 删掉刚提交的目录再报错, 避免"存在却不被本次会话
       识别"的中间状态。 */
    if (!bongo_cat_import_package_index_add(session->packages, install.id,
            normalized, metadata, error)) {
        free(normalized);
        free(metadata);
        cleanup(&install);
        return error && error->code ? error->code : BONGO_CAT_ERROR_FORMAT;
    }
    fill_receipt(receipt, install.id, normalized->count, true);
    free(normalized);
    free(metadata);
    return BONGO_CAT_OK;
}
