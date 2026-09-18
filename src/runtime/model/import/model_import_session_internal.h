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
BongoCatResult bongo_cat_import_install_discovery(
    BongoCatImportSession *session,
    BongoCatImportDiscovery *source_discovery,
    BongoCatPackageMetadata *source_metadata,
    BongoCatImportReceipt *receipt, BongoCatError *error);

#endif
