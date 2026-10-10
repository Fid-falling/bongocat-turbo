/*
 * 就近模型的缓存层: 在适配器目录里放一个 .bongo-cat-cache.json 标记, 记下来源路径、来源签名
 * (bongo_cat_nearby_signature) 与内容指纹, 让重复扫描不必重新指纹化整个模型目录; nearby 扫描在
 * 检查候选与生成适配器两步调用它, 全程在 model_import_lock 之内。
 * 契约: 缓存只是加速 —— 标记必须同时匹配来源与签名, 且适配器目录里的模式文件、适配器元数据与
 * resources 目录都还在, 任何一条不满足都退回真实指纹化。
 * 易错点: schema 版本变更会让旧标记全部失效, 这是有意的; 更新适配器走 tmp 到 old 的改名切换, 失败时必须退回旧缓存。
 */
#include "model_import_nearby_internal.h"
#include "model_storage.h"
#include "bongo_cat/json.h"
#include "bongo_cat/path.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
#include <yyjson.h>

#define NEARBY_CACHE_KIND "bongocat/nearby-model-cache"

/* 标记文件的内容。adapter_ready 为假表示这条记录只缓存了指纹 (适配器还没生成), placeholder 为真
   表示候选是内置模板, 不会出现在模型目录里。 */
typedef struct NearbyMarker {
    char source[BONGO_CAT_PATH_CAP];
    char signature[65];
    char identity[65];
    bool adapter_ready;
    bool placeholder;
    bool valid;
} NearbyMarker;

/* 只接受 64 位小写十六进制; 大小写或长度的差异都视为脏数据, 直接让标记失效。 */
static bool digest_valid(const char *value) {
    if (!value || strlen(value) != 64) return false;
    for (size_t i = 0; i < 64; ++i)
        if (!((value[i] >= '0' && value[i] <= '9') ||
            (value[i] >= 'a' && value[i] <= 'f'))) return false;
    return true;
}

/* 读并校验标记; 任何字段缺失、类型不符、schemaVersion 不符或 kind 不对, valid 都为 false (调用方
   据此退回真实计算)。返回结构体而非指针, 字符串已拷贝, 不与 json 文档共享内存。 */
static NearbyMarker read_marker(const char *target) {
    NearbyMarker marker = {0};
    char path[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(path, sizeof(path), target,
        BONGO_CAT_NEARBY_CACHE_MARKER)) return marker;
    yyjson_doc *document = bongo_cat_json_read_file(path, 0, NULL);
    yyjson_val *root = document ? yyjson_doc_get_root(document) : NULL;
    const char *kind = yyjson_get_str(yyjson_obj_get(root, "kind"));
    const char *source = yyjson_get_str(yyjson_obj_get(root, "source"));
    const char *signature = yyjson_get_str(yyjson_obj_get(root, "signature"));
    const char *identity = yyjson_get_str(yyjson_obj_get(root, "identity"));
    yyjson_val *adapter_ready = yyjson_obj_get(root, "adapterReady");
    yyjson_val *placeholder = yyjson_obj_get(root, "placeholder");
    marker.valid = yyjson_is_obj(root) && kind && source && signature &&
        digest_valid(signature) && digest_valid(identity) &&
        yyjson_is_bool(adapter_ready) && yyjson_is_bool(placeholder) &&
        yyjson_get_int(yyjson_obj_get(root, "schemaVersion")) ==
            BONGO_CAT_NEARBY_CACHE_SCHEMA &&
        strcmp(kind, NEARBY_CACHE_KIND) == 0;
    if (marker.valid) {
        snprintf(marker.source, sizeof(marker.source), "%s", source);
        snprintf(marker.signature, sizeof(marker.signature), "%s", signature);
        snprintf(marker.identity, sizeof(marker.identity), "%s", identity);
        marker.adapter_ready = yyjson_get_bool(adapter_ready);
        marker.placeholder = yyjson_get_bool(placeholder);
    }
    yyjson_doc_free(document);
    return marker;
}

/* 适配器目录能否直接使用: 标记有效且已就绪、非占位、来源一致, 并且磁盘上的模式文件、适配器元数据
   与 resources 目录都还在 (用户可能手动删掉了其中一个)。 */
static bool adapter_usable(const char *target, const char *source,
    const NearbyMarker *marker) {
    char mode[BONGO_CAT_PATH_CAP], metadata[BONGO_CAT_PATH_CAP];
    char resources[BONGO_CAT_PATH_CAP];
    return marker && marker->valid && marker->adapter_ready &&
        !marker->placeholder &&
        strcmp(marker->source, source) == 0 &&
        bongo_cat_path_is_dir(target) &&
        bongo_cat_path_join(mode, sizeof(mode), target, ".bongo-cat-mode") &&
        bongo_cat_path_join(metadata, sizeof(metadata), target,
            BONGO_CAT_MODEL_ADAPTER_FILE) &&
        bongo_cat_path_join(resources, sizeof(resources), target, "resources") &&
        bongo_cat_path_is_file(mode) && bongo_cat_path_is_file(metadata) &&
        bongo_cat_path_is_dir(resources);
}

/* 命中时回填缓存的 identity 与 placeholder 并返回 true; 来源或签名不一致返回 false, 调用方此时
   必须真做一次 bongo_cat_import_candidate_inspect。只读, 不写标记。 */
bool bongo_cat_nearby_cached_inspection(const char *target,
    const char *source, const char *signature, char identity[65],
    bool *placeholder) {
    NearbyMarker marker = read_marker(target);
    if (!marker.valid || strcmp(marker.source, source) != 0 ||
        strcmp(marker.signature, signature) != 0) return false;
    snprintf(identity, 65, "%s", marker.identity);
    if (placeholder) *placeholder = marker.placeholder;
    return true;
}

/* 原子写标记: 建 json 文档后整文件写出; 失败时由调用方清理临时目录。adapter_ready 由调用方显式
   传入 (记指纹时是 false, 适配器建好后是 true)。 */
static bool write_marker(const char *target, const char *source,
    const char *signature, const char *identity, BongoCatModelMode mode,
    bool placeholder, bool adapter_ready) {
    yyjson_mut_doc *document = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = document ? yyjson_mut_obj(document) : NULL;
    if (document) yyjson_mut_doc_set_root(document, root);
    char path[BONGO_CAT_PATH_CAP];
    bool ok = root && yyjson_mut_obj_add_int(document, root, "schemaVersion",
            BONGO_CAT_NEARBY_CACHE_SCHEMA) &&
        yyjson_mut_obj_add_str(document, root, "kind", NEARBY_CACHE_KIND) &&
        yyjson_mut_obj_add_strcpy(document, root, "source", source) &&
        yyjson_mut_obj_add_strcpy(document, root, "signature", signature) &&
        yyjson_mut_obj_add_strcpy(document, root, "identity", identity) &&
        yyjson_mut_obj_add_bool(document, root, "placeholder", placeholder) &&
        yyjson_mut_obj_add_bool(document, root, "adapterReady",
            adapter_ready) &&
        yyjson_mut_obj_add_strcpy(document, root, "mode",
            bongo_cat_mode_name(mode)) &&
        bongo_cat_path_join(path, sizeof(path), target,
            BONGO_CAT_NEARBY_CACHE_MARKER) &&
        bongo_cat_json_write_file(path, document, YYJSON_WRITE_PRETTY, NULL);
    yyjson_mut_doc_free(document);
    return ok;
}

/* 只记指纹, 不生成适配器 (adapter_ready 写 false), 目的是让下次扫描跳过昂贵的目录指纹化; 失败只
   记警告 —— 缓存写不进去不该让扫描失败。 */
void bongo_cat_nearby_remember_inspection(const char *target,
    const char *source, const char *signature, const char *identity,
    bool placeholder, BongoCatModelMode mode) {
    if ((bongo_cat_path_is_dir(target) ||
            bongo_cat_path_create_directory(target)) &&
        write_marker(target, source, signature, identity, mode,
            placeholder, false)) return;
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
        "Cannot cache nearby model identity: %s", source);
}

/* 适配器更新失败时的兜底: 旧适配器还在就沿用它并清掉 error, 让模型继续可用; 返回 false 表示连旧
   缓存都没有, 调用方必须把失败上报。 */
static bool use_previous_cache(const char *target, const char *source,
    char identity[65], BongoCatError *error) {
    NearbyMarker marker = read_marker(target);
    if (!adapter_usable(target, source, &marker)) return false;
    snprintf(identity, 65, "%s", marker.identity);
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
        "Cannot refresh nearby model cache; using the previous cache: %s",
        error && error->message[0] ? error->message : source);
    if (error) *error = (BongoCatError){0};
    return true;
}

/* 生成或更新某个候选的适配器。先看现有标记是否已与目标签名和 identity 一致 (一致就直接复用, 不
   重建), 否则在 .tmp 目录里生成, 再用 .old 换出旧目录后改名激活。
   *created 只在原本没有适配器目录时置真; adapter 始终写最终目标路径。失败时清掉临时目录、尽力
   恢复旧适配器, 并退回 use_previous_cache。 */
bool bongo_cat_nearby_refresh_cache(
    const BongoCatImportCandidate *candidate, const char *cache_root,
    const char *id, const char *source, const char *signature,
    char identity[65], bool placeholder,
    char adapter[BONGO_CAT_PATH_CAP], bool *created, BongoCatError *error) {
    char temporary[BONGO_CAT_PATH_CAP], backup[BONGO_CAT_PATH_CAP];
    char name[BONGO_CAT_ID_CAP + 8];
    snprintf(name, sizeof(name), ".%s.tmp", id);
    if (!bongo_cat_path_join(adapter, BONGO_CAT_PATH_CAP, cache_root, id) ||
        !bongo_cat_path_join(temporary, sizeof(temporary), cache_root, name))
        return false;
    snprintf(name, sizeof(name), ".%s.old", id);
    if (!bongo_cat_path_join(backup, sizeof(backup), cache_root, name))
        return false;
    bongo_cat_model_remove_tree(temporary, NULL);
    if (!bongo_cat_path_is_dir(adapter) && bongo_cat_path_is_dir(backup))
        bongo_cat_path_rename(backup, adapter);
    /* 上次更新正好在这里被打断会留下 .old 而没有正式目录, 先把它恢复成正式目录再判断要不要重建,
       免得用户看到模型凭空消失。 */
    if (bongo_cat_path_is_dir(adapter))
        bongo_cat_model_remove_tree(backup, NULL);
    *created = !bongo_cat_path_is_dir(adapter);
    NearbyMarker marker = read_marker(adapter);
    if (adapter_usable(adapter, source, &marker) &&
        strcmp(marker.signature, signature) == 0 &&
        strcmp(marker.identity, identity) == 0) return true;
    if (!bongo_cat_path_create_directory(temporary) ||
        !bongo_cat_import_prepare_adapter(candidate, temporary, error) ||
        !write_marker(temporary, source, signature, identity,
            candidate->mode, placeholder, true)) {
        bongo_cat_model_remove_tree(temporary, NULL);
        if (error && !error->message[0]) bongo_cat_error_set(error,
            BONGO_CAT_ERROR_IO, "Cannot build nearby model adapter: %s", source);
        return use_previous_cache(adapter, source, identity, error);
    }
    bool had_adapter = bongo_cat_path_is_dir(adapter);
    if (had_adapter && !bongo_cat_path_rename(adapter, backup)) {
        bongo_cat_model_remove_tree(temporary, NULL);
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Cannot update nearby model adapter: %s", SDL_GetError());
        return use_previous_cache(adapter, source, identity, error);
    }
    if (!bongo_cat_path_rename(temporary, adapter)) {
        if (had_adapter) bongo_cat_path_rename(backup, adapter);
        bongo_cat_model_remove_tree(temporary, NULL);
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Cannot activate nearby model adapter: %s", SDL_GetError());
        return use_previous_cache(adapter, source, identity, error);
    }
    bongo_cat_model_remove_tree(backup, NULL);
    return true;
}
