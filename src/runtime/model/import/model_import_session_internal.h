/*
 * 导入会话的内部契约(不对外暴露): 会话结构本身, 以及只在本模块内使用的包索引与安装事务接口。
 * 位置: 导入批次内由 model_import_session.c 创建并持有, packages 索引只是会话开始那一刻的快照。
 * 最容易改错: 这里加字段或改语义后要同步 model_import_session.c 与 model_import_install.c。
 */
#ifndef BONGO_CAT_MODEL_IMPORT_SESSION_INTERNAL_H
#define BONGO_CAT_MODEL_IMPORT_SESSION_INTERNAL_H

#include "model_import.h"

typedef struct BongoCatImportPackageIndex BongoCatImportPackageIndex;

struct BongoCatImportSession {
    char root[BONGO_CAT_PATH_CAP];
    BongoCatImportDigestCache *digests;
    BongoCatImportPackageIndex *packages;
};

BongoCatImportPackageIndex *bongo_cat_import_package_index_create(
    const char *root, BongoCatImportDigestCache *digests,
    BongoCatError *error);
void bongo_cat_import_package_index_destroy(
    BongoCatImportPackageIndex *index);
bool bongo_cat_import_package_index_find(
    const BongoCatImportPackageIndex *index,
    const BongoCatImportDiscovery *discovery,
    const BongoCatPackageMetadata *metadata,
    char ids[BONGO_CAT_IMPORT_CANDIDATE_CAP][BONGO_CAT_ID_CAP],
    size_t *count);
bool bongo_cat_import_package_index_has_capacity(
    const BongoCatImportPackageIndex *index, size_t model_count);
bool bongo_cat_import_package_index_add(BongoCatImportPackageIndex *index,
    const char *id, const BongoCatImportDiscovery *discovery,
    const BongoCatPackageMetadata *metadata, BongoCatError *error);
/* 前提: 调用方持存储锁, 且 metadata 已与 discovery 的候选按下标对齐(见 model_import_install.c)。 */
BongoCatResult bongo_cat_import_install_discovery(
    BongoCatImportSession *session,
    BongoCatImportDiscovery *source_discovery,
    BongoCatPackageMetadata *source_metadata,
    BongoCatImportReceipt *receipt, BongoCatError *error);

#endif
