/* 内容指纹用的路径工具：分隔符归一（'/' 与 '\' 等价）、Windows 上大小写不敏感的前缀包含
 * 判断，以及 FNV-1a 文本哈希与 64 位混淆函数。
 * 这些规则直接参与 digest 计算，改了任何一条比较语义等于改了所有模型的 content_digest。 */
#include "model_import_digest_internal.h"

#include <string.h>

static bool separator(char value) {
    return value == '/' || value == '\\';
}

static bool character_equal(char left, char right) {
    if (separator(left) && separator(right)) return true;
#ifdef _WIN32
    if (left >= 'A' && left <= 'Z') left = (char)(left - 'A' + 'a');
    if (right >= 'A' && right <= 'Z') right = (char)(right - 'A' + 'a');
#endif
    return left == right;
}

size_t bongo_cat_import_digest_path_length(const char *path) {
    size_t length = path ? strlen(path) : 0;
    while (length > 1 && separator(path[length - 1])) length--;
    return length;
}

/* 判断 path 是否等于 root 或位于 root 之下，且边界必须是分隔符（否则 "model" 会匹配上
 * "model2"）；root 为空视为不匹配，两端都允许带尾部分隔符。 */
bool bongo_cat_import_digest_path_contains(const char *root,
    const char *path) {
    size_t root_length = bongo_cat_import_digest_path_length(root);
    size_t item_length = bongo_cat_import_digest_path_length(path);
    if (!root_length || item_length < root_length) return false;
    for (size_t i = 0; i < root_length; ++i)
        if (!character_equal(root[i], path[i])) return false;
    return item_length == root_length || separator(path[root_length]);
}

/* FNV-1a 文本哈希，反斜杠归一成斜杠，但不折叠大小写 —— 需要 Windows 大小写归一的地方
 * （例如 digest_cache.c 里的 hash_path）要在调用之外自己处理。 */
uint64_t bongo_cat_import_digest_hash_text(const char *value) {
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char *cursor = (const unsigned char *)value;
        cursor && *cursor; ++cursor) {
        unsigned char byte = *cursor == '\\' ? '/' : *cursor;
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

/* splitmix64 的收尾混合：把相邻或相关的小整数打散，避免 size 这类值在异或聚合里互相抵消。 */
uint64_t bongo_cat_import_digest_mix(uint64_t value) {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ull;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}
