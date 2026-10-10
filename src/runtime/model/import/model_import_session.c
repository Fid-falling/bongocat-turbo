/*
 * 导入会话: 持有 models_root、摘要缓存与已安装包索引; 一个批次共用一个会话, 不是线程安全的,
 * 也不能跨批次留着 —— 包索引只是建会话那一刻的快照, 必须由同一线程串行使用。
 * 时序: 工作线程 create(建目录、清理残留 .import-*.tmp、建包索引) -> 每个来源 install -> destroy。
 * 契约: 入口自己获取全局存储锁且锁不可重入, 调用方不得持锁再进来; install 返回 OK 只表示没
 * 出错, 真正新增了几个模型看 receipt->installed_count。
 * 最容易改错: 建会话的每一步失败都要走 session_destroy 回收已建好的部分, 别直接 free。
 */
#include "model_import.h"
#include "model_import_archive.h"
#include "model_import_lock.h"
#include "model_import_session_internal.h"
#include "model_storage.h"
#include "bongo_cat/path.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 建会话的实际工作, 调用方必须已持存储锁(清理残留临时目录与扫描 models_root 建索引都在锁内)。
 * 失败时回收半成品并返回 NULL, 错误细节写进 error; 副作用是会在 models_root 下建目录, 并删除
 * 上次会话遗留的 .import-*.tmp。
 * 注意: 摘要缓存在别的调用点属于可选参数(传 NULL 表示不缓存), 会话却把它当硬依赖, 为空即失败。 */
static BongoCatImportSession *session_create_unlocked(const char *models_root,
    BongoCatError *error) {
    uint64_t started = SDL_GetTicksNS();
    SDL_Log("[runtime] Model import session creation started: root=%s",
        models_root);
    BongoCatImportSession *session = calloc(1, sizeof(*session));
    if (!session) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY,
            "Cannot allocate model import session");
        return NULL;
    }
    session->digests = bongo_cat_import_digest_cache_create();
    int root_length = snprintf(session->root, sizeof(session->root), "%s",
        models_root);
    if (!session->digests || root_length < 0 ||
        (size_t)root_length >= sizeof(session->root) ||
        !bongo_cat_path_create_directory(session->root)) {
        bongo_cat_import_session_destroy(session);
        return NULL;
    }
    SDL_Log("[runtime] Model import session creation: stage=cleanup root=%s",
        session->root);
    if (!bongo_cat_model_cleanup_imports(session->root, error)) {
        bongo_cat_import_session_destroy(session);
        return NULL;
    }
    SDL_Log("[runtime] Model import session creation: stage=index root=%s",
        session->root);
    session->packages = bongo_cat_import_package_index_create(session->root,
        session->digests, error);
    if (!session->packages) {
        bongo_cat_import_session_destroy(session);
        return NULL;
    }
    SDL_Log("[runtime] Model import session creation completed: "
        "elapsed_ms=%.1f root=%s",
        (SDL_GetTicksNS() - started) / 1000000.0, session->root);
    return session;
}

/* 加锁外壳: 锁只在整个建会话期间持有, 之后会话字段不再受锁保护, 靠"同一线程串行使用"这条约定。
 * models_root 为 NULL 时直接返回 NULL 且不写 error, 调用方要自己按参数错误区分。 */
BongoCatImportSession *bongo_cat_import_session_create(const char *models_root,
    BongoCatError *error) {
    if (!models_root) return NULL;
    SDL_Log("[runtime] Model import session lock: stage=waiting");
    bongo_cat_import_storage_lock();
    SDL_Log("[runtime] Model import session lock: stage=acquired");
    BongoCatImportSession *session = session_create_unlocked(models_root,
        error);
    bongo_cat_import_storage_unlock();
    SDL_Log("[runtime] Model import session lock: stage=released result=%d",
        session != NULL);
    return session;
}

/* 释放会话及其摘要缓存与包索引; 接受 NULL。不会删除 models_root 下的任何模型目录。 */
void bongo_cat_import_session_destroy(BongoCatImportSession *session) {
    if (!session) return;
    bongo_cat_import_package_index_destroy(session->packages);
    bongo_cat_import_digest_cache_destroy(session->digests);
    free(session);
}

/* 单个来源的安装流程, 前提是调用方已持存储锁。先把来源归一化成目录, 再发现候选, 算内容摘要
 * (会话缓存命中可跳过哈希), 最后交给 install_discovery 做事务提交。
 * discovery 与 metadata 都是本函数的堆工作区, 每条返回路径都要释放; receipt 可以为 NULL。
 * 日志里的 elapsed_ms 一律相对函数入口计时, 读日志时别当成单阶段耗时。 */
static BongoCatResult session_install_unlocked(
    BongoCatImportSession *session, const char *source,
    BongoCatImportReceipt *receipt, BongoCatError *error) {
    char source_directory[BONGO_CAT_PATH_CAP];
    uint64_t started = SDL_GetTicksNS();
    BongoCatResult source_result = bongo_cat_import_source_directory(source,
        source_directory, sizeof(source_directory), error);
    if (source_result != BONGO_CAT_OK) return source_result;
    BongoCatImportDiscovery *discovery = calloc(1, sizeof(*discovery));
    BongoCatPackageMetadata *metadata = calloc(BONGO_CAT_IMPORT_CANDIDATE_CAP,
        sizeof(*metadata));
    if (!discovery || !metadata) {
        free(discovery);
        free(metadata);
        bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY,
            "Cannot allocate model import workspace");
        return BONGO_CAT_ERROR_MEMORY;
    }
    SDL_Log("[runtime] Model import package discovery started: path=%s",
        source_directory);
    bool discovered = bongo_cat_import_discover(source_directory, discovery,
        error);
    SDL_Log("[runtime] Model import package discovery completed: result=%d "
        "candidates=%llu elapsed_ms=%.1f path=%s", discovered,
        (unsigned long long)discovery->count,
        (SDL_GetTicksNS() - started) / 1000000.0, source_directory);
    /* discovered 为假时不再算摘要: 空 discovery 上重算只会把真正的识别错误覆盖成"没有候选"。 */
    bool metadata_ready = discovered &&
        bongo_cat_import_prepare_package_metadata_cached(discovery, metadata,
            session->digests, error);
    SDL_Log("[runtime] Model import package fingerprint completed: result=%d "
        "candidates=%llu elapsed_ms=%.1f path=%s", metadata_ready,
        (unsigned long long)discovery->count,
        (SDL_GetTicksNS() - started) / 1000000.0, source_directory);
    if (!metadata_ready) {
        BongoCatResult result = error && error->code ? error->code :
            BONGO_CAT_ERROR_FORMAT;
        free(discovery);
        free(metadata);
        return result;
    }
    BongoCatResult result = bongo_cat_import_install_discovery(session,
        discovery, metadata, receipt, error);
    SDL_Log("[runtime] Model import package completed: result=%d variants=%llu "
        "installed=%llu elapsed_ms=%.1f path=%s", (int)result,
        (unsigned long long)(receipt ? receipt->count : 0),
        (unsigned long long)(receipt ? receipt->installed_count : 0),
        (SDL_GetTicksNS() - started) / 1000000.0, source_directory);
    free(discovery);
    free(metadata);
    return result;
}

/* 公开入口: 先清零 receipt(失败也让调用方读到 0), 然后全程持存储锁; zip 来源在锁内解压到临时
 * 目录, 安装结束(含失败路径)立刻清理解压树。 */
BongoCatResult bongo_cat_import_session_install(
    BongoCatImportSession *session, const char *source,
    BongoCatImportReceipt *receipt, BongoCatError *error) {
    if (receipt) memset(receipt, 0, sizeof(*receipt));
    if (!session || !source) return BONGO_CAT_ERROR_ARGUMENT;
    SDL_Log("[runtime] Model import storage lock: stage=waiting path=%s",
        source);
    bongo_cat_import_storage_lock();
    SDL_Log("[runtime] Model import storage lock: stage=acquired path=%s",
        source);
    char directory[BONGO_CAT_PATH_CAP], temporary[BONGO_CAT_PATH_CAP] = "";
    BongoCatResult result = BONGO_CAT_OK;
    bool archive = bongo_cat_import_is_archive(source);
    if (archive) result = bongo_cat_import_archive_extract(source, directory,
        temporary, error);
    if (result == BONGO_CAT_OK)
        result = session_install_unlocked(session, archive ? directory : source,
            receipt, error);
    bongo_cat_import_archive_cleanup(temporary);
    bongo_cat_import_storage_unlock();
    SDL_Log("[runtime] Model import storage lock: stage=released result=%d "
        "path=%s", (int)result, source);
    return result;
}

/* 一次性入口: 为单个来源临时建会话再销毁, 适合测试或零星导入; 批量导入请复用会话, 否则每个
 * 来源都要重建包索引, 等于把 models_root 重新扫一遍。 */
BongoCatResult bongo_cat_import_install(const char *source,
    const char *models_root, BongoCatImportReceipt *receipt,
    BongoCatError *error) {
    if (receipt) memset(receipt, 0, sizeof(*receipt));
    if (!source || !models_root) return BONGO_CAT_ERROR_ARGUMENT;
    BongoCatImportSession *session = bongo_cat_import_session_create(
        models_root, error);
    if (!session) return error && error->code ? error->code :
        BONGO_CAT_ERROR_IO;
    BongoCatResult result = bongo_cat_import_session_install(session,
        source, receipt, error);
    bongo_cat_import_session_destroy(session);
    return result;
}
