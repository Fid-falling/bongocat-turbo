/*
 * 已安装目录扫描: 把 models_root 下的用户包适配进内存 catalog, 适配器落在 cache 的 model-adapters
 * 下; 条目只在内存里, 每次扫描重建, 时序上紧随内置模型扫描之后(单包重扫由刷新线程调用)。
 * 契约: 两个入口都自己获取存储锁(锁不可重入, 调用方不得持锁进来), 且包是原子的 —— 一个包要么
 * 整体进 catalog, 要么被完整回滚, 一个坏包只记日志跳过, 不影响其它模型。
 * 最容易改错: 回滚靠"条目连续追加"这一前提, 中途插入式修改会让 memset 删错条目; describe_entry
 * 与 nearby 的 describe_nearby_entry 是两份实现, 字段要同步改。
 */
#include "model_import.h"
#include "model_import_lock.h"
#include "model_import_mver_policy.h"
#include "model_import_nearby_internal.h"
#include "runtime.h"
#include "bongo_cat/path.h"
#include "bongo_cat/sha256.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct InstalledModelScan {
    BongoCatApp *app;
    const char *cache_root;
    BongoCatError *error;
    BongoCatResult result;
} InstalledModelScan;

/* catalog 里是否已有同模式同内容摘要的条目(catalog 为 NULL 返回 false); 摘要为空的条目表示内容
 * 未知, 不参与判重。比 nearby 的同名函数少一个 ignored 参数: 这里总是在摘掉旧条目之后调用。 */
static bool existing_identity(const BongoCatModelCatalog *models,
    const char *identity, BongoCatModelMode mode) {
    for (size_t i = 0; models && i < models->count; ++i) {
        const BongoCatModelEntry *entry = &models->entries[i];
        if (entry->mode == mode && entry->content_digest[0] &&
            strcmp(entry->content_digest, identity) == 0) return true;
    }
    return false;
}

/* 同一批扫描内的跨包判重: identities/modes 是本次扫描已经发出的 (摘要, 模式) 列表, 容量就是
 * 候选上限; 重复的包不追加条目, 只落进适配器缓存。 */
static bool package_identity_seen(
    const char identities[BONGO_CAT_IMPORT_CANDIDATE_CAP][65],
    const BongoCatModelMode modes[BONGO_CAT_IMPORT_CANDIDATE_CAP],
    size_t count, const char *identity, BongoCatModelMode mode) {
    for (size_t i = 0; i < count; ++i)
        if (modes[i] == mode && strcmp(identities[i], identity) == 0)
            return true;
    return false;
}

static BongoCatModelSourceFormat source_format(
    const BongoCatImportCandidate *candidate) {
    if (candidate->format == BONGO_CAT_IMPORT_MVER_PATCH)
        return BONGO_CAT_MODEL_SOURCE_MVER_PATCH;
    return candidate->format == BONGO_CAT_IMPORT_TAURI
        ? BONGO_CAT_MODEL_SOURCE_TAURI : BONGO_CAT_MODEL_SOURCE_MVER;
}

/* 多变体包显示名的后缀: 补丁候选用补丁目录名(用户看得见的那个), 否则退回模式名。 */
static const char *variant_name(const BongoCatImportCandidate *candidate) {
    if (candidate->patch_root[0]) {
        const char *name = bongo_cat_path_name(candidate->patch_root);
        if (name && name[0]) return name;
    }
    return bongo_cat_mode_name(candidate->mode);
}

/* 用已算好的内容标识填充 catalog 条目; display 由调用方决定(单变体用包名, 多变体带后缀)。
 * 本函数负责 memset 并写 display_name, 这两件事 nearby 的同名实现都不做(那边由调用方 memset,
 * 另外还会设置 managed), 两处字段要一起改。只写内存字段, 不碰磁盘。 */
static void describe_entry(BongoCatModelEntry *entry,
    const BongoCatImportCandidate *candidate, const char *id,
    const char *identity, const char *family, const char *storage,
    const char *adapter, const char *display) {
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->id, sizeof(entry->id), "%s", id);
    snprintf(entry->package_id, sizeof(entry->package_id), "%s", id);
    snprintf(entry->content_digest, sizeof(entry->content_digest), "%s",
        identity);
    snprintf(entry->family_id, sizeof(entry->family_id), "%s", family);
    snprintf(entry->display_name, sizeof(entry->display_name), "%s", display);
    snprintf(entry->directory, sizeof(entry->directory), "%s",
        candidate->directory);
    snprintf(entry->adapter_directory, sizeof(entry->adapter_directory), "%s",
        adapter);
    snprintf(entry->storage_directory, sizeof(entry->storage_directory), "%s",
        storage);
    snprintf(entry->setting_file, sizeof(entry->setting_file), "%s",
        candidate->setting);
    entry->mode = candidate->mode;
    entry->source_format = source_format(candidate);
    entry->capabilities = bongo_cat_import_candidate_capabilities(candidate);
    entry->adapter_schema = BONGO_CAT_MODEL_ADAPTER_SCHEMA;
    entry->adapter_generator = BONGO_CAT_MODEL_ADAPTER_GENERATOR;
}

/* 把一个已安装目录的全部候选加进 catalog, 顺序是: 跳过已被完整基础包覆盖的补丁 -> 算签名与
 * 内容标识(可命中适配器缓存) -> 跳过硬编码占位模板 -> 按 (模式, 摘要) 去重 -> 生成变体 id ->
 * 跳过用户已删除的 id -> 刷新适配器缓存 -> 追加条目。
 * 返回 false 表示该包整体不可用, 调用方必须回滚本次已追加的条目; 错误写进 scan->error。
 * 最容易改错: emitted 在占位模板之后自增, 被"已删除/重复"跳过的候选仍占用编号, 这样同一目录
 * 两次扫描的变体 id 才稳定 —— 把它挪到 continue 之后会让 id 漂移。 */
static bool add_package(InstalledModelScan *scan, const char *directory,
    BongoCatImportDiscovery *discovery) {
    const char *folder = bongo_cat_path_name(directory);
    char package_id[BONGO_CAT_ID_CAP], path_hash[65], family[BONGO_CAT_ID_CAP];
    if (!bongo_cat_import_package_id(package_id, sizeof(package_id), folder) ||
        !package_id[0]) return false;
    bongo_cat_sha256_bytes(directory, strlen(directory), path_hash);
    snprintf(family, sizeof(family), "family-installed-%.16s", path_hash);
    size_t emitted = 0;
    char identities[BONGO_CAT_IMPORT_CANDIDATE_CAP][65] = {{0}};
    BongoCatModelMode modes[BONGO_CAT_IMPORT_CANDIDATE_CAP] = {0};
    size_t identity_count = 0;
    for (size_t i = 0; i < discovery->count; ++i) {
        if (bongo_cat_import_patch_has_full_base(discovery, i)) continue;
        BongoCatImportCandidate *candidate = &discovery->candidates[i];
        char cache_id[BONGO_CAT_ID_CAP], adapter[BONGO_CAT_PATH_CAP];
        char signature[65], identity[65];
        snprintf(cache_id, sizeof(cache_id), "installed-%.16s-%zu",
            path_hash, i + 1);
        if (!bongo_cat_path_join(adapter, sizeof(adapter), scan->cache_root,
                cache_id) ||
            !bongo_cat_nearby_signature(candidate, signature, scan->error))
            return false;
        bool placeholder = false;
        bool cached = bongo_cat_nearby_cached_inspection(adapter, directory,
            signature, identity, &placeholder);
        if (!cached && !bongo_cat_import_candidate_inspect(candidate, identity,
                &placeholder, scan->error)) return false;
        /* 内置模板占位: 不进 catalog, 只把指纹记进缓存, 下次扫描省掉一次解析。 */
        if (placeholder) {
            if (!cached) bongo_cat_nearby_remember_inspection(adapter,
                directory, signature, identity, true, candidate->mode);
            continue;
        }
        bool duplicate = existing_identity(&scan->app->models, identity,
                candidate->mode) || package_identity_seen(identities, modes,
                identity_count, identity, candidate->mode);
        if (identity_count < BONGO_CAT_IMPORT_CANDIDATE_CAP) {
            snprintf(identities[identity_count], 65, "%s", identity);
            modes[identity_count++] = candidate->mode;
        }
        char id[BONGO_CAT_ID_CAP];
        if (!bongo_cat_import_variant_id(id, sizeof(id), package_id, emitted)) {
            bongo_cat_error_set(scan->error, BONGO_CAT_ERROR_FORMAT,
                "Installed model id is invalid: %s", package_id);
            return false;
        }
        emitted++;
        if (bongo_cat_settings_model_removed(&scan->app->settings, id)) {
            if (!cached) bongo_cat_nearby_remember_inspection(adapter,
                directory, signature, identity, false, candidate->mode);
            continue;
        }
        /* 内容与目录里已有条目重复: 同样只留指纹, 不追加条目。 */
        if (duplicate) {
            if (!cached) bongo_cat_nearby_remember_inspection(adapter,
                directory, signature, identity, false, candidate->mode);
            continue;
        }
        if (scan->app->models.count >= BONGO_CAT_MODEL_CAP) {
            bongo_cat_error_set(scan->error, BONGO_CAT_ERROR_FORMAT,
                "Too many installed model variants");
            return false;
        }
        bool created = false;
        if (!bongo_cat_nearby_refresh_cache(candidate, scan->cache_root,
                cache_id, directory, signature, identity, false, adapter,
                &created, scan->error)) return false;
        (void)created;
        if (bongo_cat_models_find(&scan->app->models, id)) {
            bongo_cat_error_set(scan->error, BONGO_CAT_ERROR_FORMAT,
                "Installed model id is not unique: %s", package_id);
            return false;
        }
        char display[BONGO_CAT_ID_CAP];
        if (discovery->count > 1)
            snprintf(display, sizeof(display), "%s - %s", package_id,
                variant_name(candidate));
        else snprintf(display, sizeof(display), "%s", package_id);
        BongoCatModelEntry *entry =
            &scan->app->models.entries[scan->app->models.count++];
        describe_entry(entry, candidate, id, identity, family, directory,
            adapter, display);
    }
    return true;
}

/* 目录枚举回调: 只处理非隐藏、且未被运行时标记接管的目录(带 .bongo-cat-* 标记的目录不是用户
 * 手写的包, 交给别处处理)。识别失败只记警告跳过; add_package 失败时把本次追加的条目整体回滚
 * —— 包之间互相独立, 一个坏包不该让其它模型从列表里消失。
 * 只有路径拼接失败、内存不足这类硬错误才返回 FAILURE 终止枚举, 此时 scan->result 已设好。 */
static BongoCatPathVisit scan_package(void *userdata,
    const char *dirname, const char *name) {
    InstalledModelScan *scan = userdata;
    if (!name || name[0] == '.') return BONGO_CAT_PATH_CONTINUE;
    char directory[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(directory, sizeof(directory), dirname, name)) {
        scan->result = BONGO_CAT_ERROR_FORMAT;
        return BONGO_CAT_PATH_FAILURE;
    }
    if (!bongo_cat_import_authored_package(directory))
        return BONGO_CAT_PATH_CONTINUE;
    BongoCatImportDiscovery *discovery = calloc(1, sizeof(*discovery));
    if (!discovery) {
        scan->result = BONGO_CAT_ERROR_MEMORY;
        return BONGO_CAT_PATH_FAILURE;
    }
    BongoCatError local = {0};
    bool found = bongo_cat_import_discover(directory, discovery, &local);
    if (!found) {
        if (local.message[0]) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Skipping installed model package: path=%s error=%s",
            directory, local.message);
        free(discovery);
        return BONGO_CAT_PATH_CONTINUE;
    }
    size_t before = scan->app->models.count;
    if (scan->error) *scan->error = (BongoCatError){0};
    bool added = add_package(scan, directory, discovery);
    free(discovery);
    if (added) return BONGO_CAT_PATH_CONTINUE;
    /* A package is atomic, but independent packages remain discoverable. */
    memset(&scan->app->models.entries[before], 0,
        (scan->app->models.count - before) * sizeof(scan->app->models.entries[0]));
    scan->app->models.count = before;
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
        "Skipping installed model package: path=%s error=%s", directory,
        scan->error && scan->error->message[0] ? scan->error->message :
            "model adaptation failed");
    if (scan->error) *scan->error = (BongoCatError){0};
    return BONGO_CAT_PATH_CONTINUE;
}

/* 适配器缓存根 = app->cache_root/model-adapters, 不存在则创建; 失败统一报 IO。 */
static bool installed_cache_root(const BongoCatApp *app,
    char cache_root[BONGO_CAT_PATH_CAP], BongoCatError *error) {
    if (bongo_cat_path_join(cache_root, BONGO_CAT_PATH_CAP, app->cache_root,
            BONGO_CAT_ADAPTER_CACHE_DIRECTORY) &&
        bongo_cat_path_create_directory(cache_root)) return true;
    bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
        "Cannot create installed model adapter cache");
    return false;
}

/* 按 storage_directory 摘掉属于该目录的全部条目: 原地压缩保持顺序, 尾部槽位清零, 不留已删除
 * 条目的副本, 也不改变其它包的条目顺序。 */
static void remove_storage_models(BongoCatModelCatalog *models,
    const char *directory) {
    size_t output = 0;
    for (size_t i = 0; i < models->count; ++i) {
        BongoCatModelEntry entry = models->entries[i];
        if (!strcmp(entry.storage_directory, directory)) continue;
        models->entries[output++] = entry;
    }
    if (output < BONGO_CAT_MODEL_CAP)
        memset(&models->entries[output], 0,
            (BONGO_CAT_MODEL_CAP - output) * sizeof(models->entries[0]));
    models->count = output;
}

/* 单包重扫(删除或刷新某个模型后由刷新线程调用): 先校验 package_id 与归一化结果完全一致, 借此
 * 拒绝 "a/../b" 这类会跑出 root 的写法; 前提是 app->cache_root 已设置。
 * 副作用: 持存储锁; 识别成功时先摘掉该包的旧条目再整体重建, 所以 add_package 失败会留下"包还在
 * 磁盘上但不在 catalog 里"的状态, 要等下一次成功扫描才回来; 识别失败则保留旧条目不动。
 * 返回第一个错误码; found 为假且没有更具体的错误时兜底 FORMAT。 */
BongoCatResult bongo_cat_import_installed_package(BongoCatApp *app,
    const char *root, const char *package_id, BongoCatError *error) {
    if (!app || !root || !package_id || !package_id[0] ||
        !app->cache_root[0]) return BONGO_CAT_ERROR_ARGUMENT;
    char normalized_id[BONGO_CAT_ID_CAP];
    if (!bongo_cat_import_package_id(normalized_id, sizeof(normalized_id),
            package_id) || strcmp(normalized_id, package_id))
        return BONGO_CAT_ERROR_ARGUMENT;
    char directory[BONGO_CAT_PATH_CAP], cache_root[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(directory, sizeof(directory), root, package_id) ||
        !bongo_cat_import_authored_package(directory) ||
        !installed_cache_root(app, cache_root, error)) {
        if (error && !error->code)
            bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
                "Installed model package is unavailable: %s", package_id);
        return error && error->code ? error->code : BONGO_CAT_ERROR_FORMAT;
    }
    BongoCatImportDiscovery *discovery = calloc(1, sizeof(*discovery));
    if (!discovery) return BONGO_CAT_ERROR_MEMORY;
    bongo_cat_import_storage_lock();
    bool found = bongo_cat_import_discover(directory, discovery, error);
    InstalledModelScan scan = {app, cache_root, error, BONGO_CAT_OK};
    if (found) {
        remove_storage_models(&app->models, directory);
        if (!add_package(&scan, directory, discovery))
            scan.result = error && error->code ? error->code :
                BONGO_CAT_ERROR_IO;
    }
    bongo_cat_import_storage_unlock();
    free(discovery);
    if (!found && scan.result == BONGO_CAT_OK)
        scan.result = error && error->code ? error->code :
            BONGO_CAT_ERROR_FORMAT;
    return scan.result;
}

/* 启动扫描入口: root 不存在时返回 OK(用户还没装过模型)。返回 OK 不代表所有包都加载成功 ——
 * 单个包的失败在 scan_package 里降级成警告, 只有枚举中断这类整体故障才报错。
 * 前提: app->cache_root 已设置, 且调用方已把内置与已存模型加进 catalog(判重依赖它)。 */
BongoCatResult bongo_cat_import_installed_models(BongoCatApp *app,
    const char *root, BongoCatError *error) {
    if (!app || !root || !app->cache_root[0]) return BONGO_CAT_ERROR_ARGUMENT;
    if (!bongo_cat_path_is_dir(root)) return BONGO_CAT_OK;
    char cache_root[BONGO_CAT_PATH_CAP];
    if (!installed_cache_root(app, cache_root, error)) return BONGO_CAT_ERROR_IO;
    InstalledModelScan scan = {app, cache_root, error, BONGO_CAT_OK};
    bongo_cat_import_storage_lock();
    bool ok = bongo_cat_path_enumerate(root, scan_package, &scan);
    bongo_cat_import_storage_unlock();
    if (!ok && scan.result == BONGO_CAT_OK) scan.result = BONGO_CAT_ERROR_IO;
    return scan.result;
}
