/*
 * 会话内的"已安装包索引": 建会话时扫描 models_root 建立, 每次安装成功后再追加。
 * 用途是内容级去重 —— 摘要与模式集合都对得上的包视为同一个包, 不再重复落盘。
 * 索引是快照, 只在持有存储锁的导入流程内读写, 非线程安全。
 */
#include "model_import_session_internal.h"
#include "bongo_cat/path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 一个已安装包在索引里的紧凑表示: 包 id 加它全部变体的 (模式, 内容摘要) 集合。 */
typedef struct IndexedPackage {
    char id[BONGO_CAT_ID_CAP];
    char digests[BONGO_CAT_IMPORT_CANDIDATE_CAP][65];
    BongoCatModelMode modes[BONGO_CAT_IMPORT_CANDIDATE_CAP];
    size_t count;
} IndexedPackage;

struct BongoCatImportPackageIndex {
    IndexedPackage entries[BONGO_CAT_MODEL_CAP];
    size_t count;
    size_t model_count;
};

/* Build once for a batch; successful atomic installs extend this snapshot. */

/* 判断"已知条目"能否覆盖 discovery 的全部候选: 用 used[] 保证模式与摘要一对一(同一条目
   不能匹配两次); discovery->count 大于已知变体数时直接不匹配。matches[i] 只在返回 true
   时有意义, 是 discovery 第 i 个候选对应的变体下标。 */
static bool match_package(const IndexedPackage *known,
    const BongoCatImportDiscovery *discovery,
    const BongoCatPackageMetadata *metadata,
    size_t matches[BONGO_CAT_IMPORT_CANDIDATE_CAP]) {
    if (!known || !discovery || !metadata || discovery->count > known->count)
        return false;
    bool used[BONGO_CAT_IMPORT_CANDIDATE_CAP] = {0};
    for (size_t i = 0; i < discovery->count; ++i) {
        bool found = false;
        for (size_t j = 0; j < known->count; ++j) {
            if (used[j] || known->modes[j] != discovery->candidates[i].mode ||
                strcmp(known->digests[j], metadata[i].content_digest) != 0)
                continue;
            used[j] = true;
            matches[i] = j;
            found = true;
            break;
        }
        if (!found) return false;
    }
    return true;
}

/* 容量按"变体总数"计(BONGO_CAT_MODEL_CAP 是模型总数上限), 溢出时报 FORMAT 并且不改动
   索引; 已提交目录的回滚由调用方负责。 */
bool bongo_cat_import_package_index_add(BongoCatImportPackageIndex *index,
    const char *id, const BongoCatImportDiscovery *discovery,
    const BongoCatPackageMetadata *metadata, BongoCatError *error) {
    if (!index || !id || !id[0] || !discovery || !metadata ||
        discovery->count > BONGO_CAT_IMPORT_CANDIDATE_CAP) return false;
    if (index->count >= BONGO_CAT_MODEL_CAP ||
        discovery->count > BONGO_CAT_MODEL_CAP - index->model_count) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            "Too many installed model variants");
        return false;
    }
    IndexedPackage *entry = &index->entries[index->count++];
    entry->count = discovery->count;
    index->model_count += discovery->count;
    snprintf(entry->id, sizeof(entry->id), "%s", id);
    for (size_t i = 0; i < entry->count; ++i) {
        entry->modes[i] = discovery->candidates[i].mode;
        snprintf(entry->digests[i], sizeof(entry->digests[i]), "%s",
            metadata[i].content_digest);
    }
    return true;
}

/* 找到能覆盖全部候选的包就返回 true, 并通过 ids/count 给出该包各变体的 id(用变体下标
   还原 "~N" 后缀); 返回 false 表示"没装过", 此时 count 已被清零。 */
bool bongo_cat_import_package_index_find(
    const BongoCatImportPackageIndex *index,
    const BongoCatImportDiscovery *discovery,
    const BongoCatPackageMetadata *metadata,
    char ids[BONGO_CAT_IMPORT_CANDIDATE_CAP][BONGO_CAT_ID_CAP],
    size_t *count) {
    if (!index || !discovery || !metadata || !ids || !count ||
        discovery->count > BONGO_CAT_IMPORT_CANDIDATE_CAP) return false;
    *count = 0;
    for (size_t i = 0; i < index->count; ++i) {
        size_t matches[BONGO_CAT_IMPORT_CANDIDATE_CAP] = {0};
        if (!match_package(&index->entries[i], discovery, metadata, matches))
            continue;
        for (size_t j = 0; j < discovery->count; ++j)
            if (!bongo_cat_import_variant_id(ids[j], BONGO_CAT_ID_CAP,
                    index->entries[i].id, matches[j])) {
                *count = 0;
                return false;
            }
        *count = discovery->count;
        return true;
    }
    return false;
}

/* 供调用方在复制之前预判容量, 避免"复制完才发现放不下"。 */
bool bongo_cat_import_package_index_has_capacity(
    const BongoCatImportPackageIndex *index, size_t model_count) {
    return index && index->count < BONGO_CAT_MODEL_CAP &&
        model_count <= BONGO_CAT_MODEL_CAP - index->model_count;
}

typedef struct IndexBuild {
    BongoCatImportPackageIndex *index;
    BongoCatImportDigestCache *digests;
    BongoCatError *error;
    bool failed;
} IndexBuild;

/* 目录枚举回调: 跳过隐藏项与带运行时标记的目录; 单个坏包(识别或摘要失败)只跳过,
   不中断整个索引 —— 只有内存不足或索引写入失败才返回 FAILURE 终止建索引。 */
static BongoCatPathVisit index_package(void *userdata,
    const char *dirname, const char *name) {
    IndexBuild *build = userdata;
    if (!name || name[0] == '.') return BONGO_CAT_PATH_CONTINUE;
    char directory[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(directory, sizeof(directory), dirname, name) ||
        !bongo_cat_import_authored_package(directory))
        return BONGO_CAT_PATH_CONTINUE;
    BongoCatImportDiscovery *discovery = calloc(1, sizeof(*discovery));
    BongoCatPackageMetadata *metadata = calloc(
        BONGO_CAT_IMPORT_CANDIDATE_CAP, sizeof(*metadata));
    if (!discovery || !metadata) {
        free(discovery);
        free(metadata);
        bongo_cat_error_set(build->error, BONGO_CAT_ERROR_MEMORY,
            "Cannot index installed model packages");
        build->failed = true;
        return BONGO_CAT_PATH_FAILURE;
    }
    BongoCatError ignored = {0};
    bool valid = bongo_cat_import_discover(directory, discovery, &ignored) &&
        bongo_cat_import_prepare_package_metadata_cached(discovery, metadata,
            build->digests, &ignored);
    char id[BONGO_CAT_ID_CAP];
    bool added = !valid ||
        (bongo_cat_import_package_id(id, sizeof(id), name) &&
            bongo_cat_import_package_index_add(build->index, id, discovery,
                metadata, build->error));
    free(discovery);
    free(metadata);
    if (added) return BONGO_CAT_PATH_CONTINUE;
    build->failed = true;
    return BONGO_CAT_PATH_FAILURE;
}

/* 失败返回 NULL 并释放半成品索引; 只有枚举失败或回调显式失败才报错(单个坏包会被跳过)。 */
BongoCatImportPackageIndex *bongo_cat_import_package_index_create(
    const char *root, BongoCatImportDigestCache *digests,
    BongoCatError *error) {
    if (!root || !digests) return NULL;
    BongoCatImportPackageIndex *index = calloc(1, sizeof(*index));
    if (!index) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY,
            "Cannot allocate installed model index");
        return NULL;
    }
    IndexBuild build = {index, digests, error, false};
    bool scanned = bongo_cat_path_enumerate(root, index_package, &build);
    if (scanned && !build.failed) return index;
    if (!build.failed)
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Cannot index installed model packages");
    free(index);
    return NULL;
}

void bongo_cat_import_package_index_destroy(
    BongoCatImportPackageIndex *index) {
    free(index);
}
