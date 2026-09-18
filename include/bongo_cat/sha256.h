#ifndef BONGO_CAT_SHA256_H
#define BONGO_CAT_SHA256_H

#include "bongo_cat/common.h"

void bongo_cat_sha256_bytes(const void *data, size_t size, char output[65]);
BongoCatResult bongo_cat_sha256_file(const char *path, char output[65], BongoCatError *error);

#endif
