#ifndef BONGO_CAT_PATH_H
#define BONGO_CAT_PATH_H

#include "bongo_cat/common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum BongoCatPathVisit {
    BONGO_CAT_PATH_CONTINUE,
    BONGO_CAT_PATH_SUCCESS,
    BONGO_CAT_PATH_FAILURE
} BongoCatPathVisit;

typedef BongoCatPathVisit (*BongoCatPathVisitor)(void *userdata,
    const char *directory, const char *name);

bool bongo_cat_path_join(char *output, size_t capacity, const char *left, const char *right);
const char *bongo_cat_path_name(const char *path);
bool bongo_cat_path_is_file(const char *path);
bool bongo_cat_path_is_dir(const char *path);
bool bongo_cat_path_file_size(const char *path, uint64_t *size);
bool bongo_cat_path_file_info(const char *path, uint64_t *size, uint64_t *modified);
bool bongo_cat_path_create_directory(const char *path);
bool bongo_cat_path_enumerate(const char *path,
    BongoCatPathVisitor visitor, void *userdata);
bool bongo_cat_path_copy_file(const char *source, const char *target);
bool bongo_cat_path_rename(const char *source, const char *target);
bool bongo_cat_path_remove(const char *path);
bool bongo_cat_path_find_suffix(const char *directory, const char *suffix,
    char *name, size_t capacity);
int bongo_cat_path_find_unique_suffix(const char *directory, const char *suffix,
    char *name, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
