/* 内容指纹的内部原语（只给 import 目录内的 .c 使用）：路径前缀比较、文本哈希与混淆函数。
 * hash_text 与 path_contains 的规则直接进最终 digest，改动会让已安装模型的身份失效。 */
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
