#ifndef BONGO_CAT_FILE_H
#define BONGO_CAT_FILE_H

#include "bongo_cat/common.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

FILE *bongo_cat_file_open(const char *path, const char *mode);
bool bongo_cat_file_append(const char *path, const void *data, size_t size);
bool bongo_cat_file_remove(const char *path);
bool bongo_cat_file_replace(const char *source, const char *target, bool durable);

#ifdef __cplusplus
}
#endif

#endif
