#ifndef BONGO_CAT_MODEL_IMPORT_DIGEST_INTERNAL_H
#define BONGO_CAT_MODEL_IMPORT_DIGEST_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

size_t bongo_cat_import_digest_path_length(const char *path);
bool bongo_cat_import_digest_path_contains(const char *root,
    const char *path);
uint64_t bongo_cat_import_digest_hash_text(const char *value);
uint64_t bongo_cat_import_digest_mix(uint64_t value);

#endif
