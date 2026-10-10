/*
 * 就近模型扫描: 把可执行文件旁的 Mver 或 Tauri 模型包就地适配成运行时适配器, 原始目录保持不动,
 * 适配器落在 cache 的 model-adapters 下。
 * 位置: 启动与刷新模型时由 model_catalog_runtime.c 调用; 全程必须在 model_import_lock 之内 ——
 * 本文件只有 import_root 一层加锁, 加锁后不得再调用其它会加锁的入口 (互斥量不可重入)。
 * 契约: 返回 BONGO_CAT_OK 只表示扫描跑完, 不代表发现了模型; 单个候选的失败在 _scan 入口下只记日志
 * 跳过, 在 _root 入口下直接失败返回。
 */
#include "model_import.h"
#include "model_import_lock.h"
#include "model_import_probe.h"
#include "model_import_mver_policy.h"
#include "model_import_mver.h"
#include "model_import_nearby_internal.h"
#include "../model_import_path.h"
#include "../tauri/model_import_tauri.h"
#include "runtime.h"
#include "bongo_cat/path.h"
#include "bongo_cat/sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 限时扫描的总预算: 启动路径上宁可少发现几个模型, 也不能让扫描拖慢窗口出现。到期后
   bongo_cat_import_scan_budget 直接收工, 已经加进去的条目保留。 */
#define NEARBY_SCAN_BUDGET_NS 500000000ull

/* 目录里是否已有模式相同、内容指纹相同的模型; ignored 用于跳过正在更新的那一条自身。
   content_digest 为空的条目视为内容未知, 不参与判重。 */
static bool existing_identity(const BongoCatModelCatalog *models,
    const char *identity, BongoCatModelMode mode,
    const BongoCatModelEntry *ignored) {
    for (size_t i = 0; models && i < models->count; ++i) {
        const BongoCatModelEntry *entry = &models->entries[i];
        if (entry != ignored && entry->mode == mode &&
            entry->content_digest[0] &&
            strcmp(entry->content_digest, identity) == 0) return true;
    }
    return false;
}

static BongoCatModelEntry *model_with_id(BongoCatApp *app, const char *id) {
    for (size_t i = 0; app && i < app->models.count; ++i)
        if (strcmp(app->models.entries[i].id, id) == 0)
            return &app->models.entries[i];
    return NULL;
}

/* 候选对应的磁盘来源: 补丁用 patch_root (用户往往直接选中补丁目录), 其余用 package_root, 都没有
   时退回调用方给的 fallback。 */
static const char *candidate_source(const BongoCatImportCandidate *candidate,
    const char *fallback) {
    if (candidate->format == BONGO_CAT_IMPORT_MVER_PATCH &&
        candidate->patch_root[0]) return candidate->patch_root;
    return candidate->package_root[0] ? candidate->package_root : fallback;
}

/* 生成显示名, 一般就是来源目录名。特例: 补丁目录名与其 package_root 同名 (用户选中了完整包目录)
   时改用上一级目录名, 否则补丁会顶掉用户熟悉的名字。 */
static void candidate_name(const char *source,
    const BongoCatImportCandidate *candidate, char *name, size_t capacity) {
    const char *value = bongo_cat_path_name(source);
    char parent[BONGO_CAT_PATH_CAP];
    if (value && candidate->format == BONGO_CAT_IMPORT_MVER_PATCH &&
        candidate->package_root[0] &&
        !strcmp(value, bongo_cat_path_name(candidate->package_root)) &&
        bongo_cat_import_parent_path(source, parent, sizeof(parent)))
        value = bongo_cat_path_name(parent);
    snprintf(name, capacity, "%s", value && value[0] ? value : "Nearby model");
}

/* 把一个候选适配进模型目录。id 由来源路径哈希加模式名派生, 因此同一路径重复扫描是幂等的: 指纹与
   适配器都没变就直接复用缓存标记, 连适配器目录都不重建。同一 id 若是用户自己放进去的非 managed
   条目则原样保留。
   返回 false 表示候选不可用 (指纹无法确定或超出模型容量), 由调用方决定是致命还是跳过;
   first_created 记录本次第一个真正新建的 id, 供上层决定要不要自动切换当前模型。 */
static bool add_candidate(BongoCatApp *app, const char *cache_root,
    const char *source, const BongoCatImportCandidate *candidate,
    char first_created[BONGO_CAT_ID_CAP], BongoCatError *error) {
    char source_hash[65], signature[65], identity[65];
    char id[BONGO_CAT_ID_CAP], adapter[BONGO_CAT_PATH_CAP];
    bongo_cat_sha256_bytes(source, strlen(source), source_hash);
    snprintf(id, sizeof(id), "nearby-%.16s-%s", source_hash,
        bongo_cat_mode_name(candidate->mode));
    BongoCatModelEntry *same_id = model_with_id(app, id);
    if (same_id && !same_id->managed) return true;
    if (!bongo_cat_nearby_signature(candidate, signature, error) ||
        !bongo_cat_path_join(adapter, sizeof(adapter), cache_root, id))
        return false;
    bool placeholder = false;
    bool identity_cached = bongo_cat_nearby_cached_inspection(adapter, source,
        signature, identity, &placeholder);
    if (!identity_cached &&
        !bongo_cat_import_candidate_inspect(candidate, identity,
            &placeholder, error))
        return false;
    if (placeholder) {
        if (!identity_cached) bongo_cat_nearby_remember_inspection(adapter,
            source, signature, identity, true, candidate->mode);
        return true;
    }
    /* 补丁候选额外查一次: 它若建立在未改动的内置模型之上 (底层清单仍是官方那份), 或者底层包的
       内容已经在目录里, 这个补丁就不算独立模型, 直接沿用已有条目。 */
    if (candidate->format == BONGO_CAT_IMPORT_MVER_PATCH) {
        char base_identity[65];
        bool base_placeholder = false;
        if (!bongo_cat_import_patch_base_inspect(candidate, base_identity,
                &base_placeholder, error)) return false;
        if (base_placeholder || existing_identity(&app->models, base_identity,
                candidate->mode, same_id)) return true;
    }
    if (existing_identity(&app->models, identity, candidate->mode, same_id)) {
        if (!identity_cached) bongo_cat_nearby_remember_inspection(adapter,
            source, signature, identity, false, candidate->mode);
        return true;
    }
    /* 内容与目录里已有模型相同: 记下指纹 (但不标 adapter_ready, 表示它没有自己的适配器) 后跳过,
       下次扫描连指纹化都能省掉。 */
    if (!same_id && app->models.count >= BONGO_CAT_MODEL_CAP) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            "Too many models while adding nearby package");
        return false;
    }
    bool created = false;
    if (!bongo_cat_nearby_refresh_cache(candidate, cache_root, id, source,
        signature, identity, false, adapter, &created, error)) return false;
    if (!same_id && existing_identity(&app->models, identity,
        candidate->mode, NULL)) return true;
    BongoCatModelEntry *entry = same_id ? same_id :
        &app->models.entries[app->models.count++];
    memset(entry, 0, sizeof(*entry));
    candidate_name(source, candidate, entry->display_name,
        sizeof(entry->display_name));
    bongo_cat_import_describe_nearby_entry(entry, candidate, id, identity,
        source_hash, source, adapter);
    if (created && !first_created[0])
        snprintf(first_created, BONGO_CAT_ID_CAP, "%s", id);
    return true;
}

typedef struct NearbyAdd {
    BongoCatApp *app;
    const char *cache_root;
    char *first_created;
    bool best_effort;
} NearbyAdd;

/* 遍历发现结果里的每个候选。best_effort 为真 (即限时扫描入口) 时把单个候选的错误降级成警告日志
   后继续, 启动扫描不能因为一个坏模型全灭; 为假时立刻返回错误码, 让上层能看到原因。 */
static BongoCatResult add_discovery(NearbyAdd *add, const char *fallback,
    BongoCatImportDiscovery *discovery, BongoCatError *error) {
    for (size_t i = 0; i < discovery->count; ++i) {
        BongoCatImportCandidate *candidate = &discovery->candidates[i];
        const char *source = candidate_source(candidate, fallback);
        if (!add_candidate(add->app, add->cache_root, source, candidate,
                add->first_created, error)) {
            if (!add->best_effort) return error && error->code
                ? error->code : BONGO_CAT_ERROR_IO;
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Skipping nearby model: path=%s model=%s error=%s", source,
                candidate->directory, error && error->message[0]
                    ? error->message : "model adaptation failed");
            if (error) *error = (BongoCatError){0};
        }
    }
    return BONGO_CAT_OK;
}

static BongoCatResult add_scanned(void *userdata, const char *source,
    BongoCatImportDiscovery *discovery, BongoCatError *error) {
    return add_discovery(userdata, source, discovery, error);
}

/* 在 root 上做发现。bounded 走 probe_exact (只认精确包根, 不向上回溯) 以控制耗时; 否则按 Mver
   宽查找、Mver 补丁、Tauri 的顺序依次尝试, 每次失败都清空 discovery 与 error, 免得上一个适配器
   的残留影响下一个。 */
static int discover_direct(const char *root, bool bounded,
    BongoCatImportDiscovery *discovery, BongoCatError *error) {
    if (bounded) return bongo_cat_import_probe_exact(root, discovery,
        NULL, BONGO_CAT_IMPORT_PROBE_FALLBACK, false, error);
    /* An explicit nearby root uses broad Mver lookup so selecting an inner
       authored folder still resolves to its canonical package root. */
    int found = bongo_cat_import_mver_discover(root, discovery, error);
    if (found <= 0) {
        memset(discovery, 0, sizeof(*discovery));
        if (error) *error = (BongoCatError){0};
        found = bongo_cat_import_mver_patch_discover(root, discovery, error);
    }
    if (found <= 0) {
        memset(discovery, 0, sizeof(*discovery));
        if (error) *error = (BongoCatError){0};
        found = bongo_cat_import_tauri_discover_exact(root,
            discovery, error);
    }
    return found;
}

/* 加锁内的主流程: 建缓存目录, 先试精确发现, 找不到再做限时或完整的递归扫描。成功后若当前选中项
   为空或还是内置默认 id, 就把选中项切到本次新建的第一个模型。
   前提: app 与 root 非空且 app->cache_root 已设置, root 不存在时直接返回 OK。
   discovery 是本函数堆分配并释放的临时工作区, 不返回给调用方。 */
static BongoCatResult import_root_unlocked(BongoCatApp *app, const char *root,
    bool bounded, BongoCatError *error) {
    if (!app || !root || !app->cache_root[0])
        return BONGO_CAT_ERROR_ARGUMENT;
    if (!bongo_cat_path_is_dir(root)) return BONGO_CAT_OK;
    char cache_root[BONGO_CAT_PATH_CAP];
    char first_created[BONGO_CAT_ID_CAP] = {0};
    if (!bongo_cat_path_join(cache_root, sizeof(cache_root), app->cache_root,
            BONGO_CAT_ADAPTER_CACHE_DIRECTORY) ||
        !bongo_cat_path_create_directory(cache_root))
        return BONGO_CAT_ERROR_IO;
    BongoCatImportDiscovery *discovery = calloc(1, sizeof(*discovery));
    if (!discovery) return BONGO_CAT_ERROR_MEMORY;
    int direct = discover_direct(root, bounded, discovery, error);
    NearbyAdd add = {app, cache_root, first_created, bounded};
    BongoCatResult result = direct > 0
        ? add_discovery(&add, root, discovery, error) : BONGO_CAT_OK;
    if (direct <= 0) {
        if (error) *error = (BongoCatError){0};
        result = bounded
            ? bongo_cat_import_scan_budget(root, add_scanned, &add,
                NEARBY_SCAN_BUDGET_NS, error)
            : bongo_cat_import_scan(root, add_scanned, &add, error);
    }
    const char *selected = app->session.active_model_id;
    if (first_created[0] && (!selected[0] || !strcmp(selected, "standard") ||
        !strcmp(selected, "keyboard") || !strcmp(selected, "gamepad")))
        snprintf(app->session.active_model_id,
            sizeof(app->session.active_model_id), "%s", first_created);
    free(discovery);
    return result;
}

/* 加锁外壳: 模型目录的扫描与写盘共用一个互斥量, 因此这里不能嵌套调用另一个会加锁的入口 (会自
   死锁)。 */
static BongoCatResult import_root(BongoCatApp *app, const char *root,
    bool bounded, BongoCatError *error) {
    bongo_cat_import_storage_lock();
    BongoCatResult result = import_root_unlocked(app, root, bounded, error);
    bongo_cat_import_storage_unlock();
    return result;
}

/* 非限时入口: 精确发现加完整递归扫描, 单个候选的错误视为致命。 */
BongoCatResult bongo_cat_import_nearby_root(BongoCatApp *app,
    const char *root, BongoCatError *error) {
    return import_root(app, root, false, error);
}

/* 限时入口: 给启动路径用, 预算 500 ms, 单个候选失败只记日志。 */
BongoCatResult bongo_cat_import_nearby_scan(BongoCatApp *app,
    const char *root, BongoCatError *error) {
    return import_root(app, root, true, error);
}

/* 面向用户选中目录的入口: 先扫选中目录, 一个模型都没新增时再扫它的父目录 —— 用户常常只选中包里
   的 img 或子目录, 父目录才是包根。 */
BongoCatResult bongo_cat_import_nearby(BongoCatApp *app,
    const char *root, BongoCatError *error) {
    if (!app || !root) return BONGO_CAT_ERROR_ARGUMENT;
    size_t before = app->models.count;
    BongoCatResult result = bongo_cat_import_nearby_root(app, root, error);
    if (result != BONGO_CAT_OK || app->models.count != before) return result;
    char parent[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_import_parent_path(root, parent, sizeof(parent)) ||
        !strcmp(parent, root))
        return BONGO_CAT_OK;
    if (error) *error = (BongoCatError){0};
    return bongo_cat_import_nearby_root(app, parent, error);
}
