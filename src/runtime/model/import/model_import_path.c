/* 源格式模块共用的小路径工具：取父目录、取相对路径、后缀比较。
 * 与前缀有关的判断在 Windows 上折叠大小写并把 '/' 与 '\' 视为等价（导入源可能是别的平台
 * 打出来的包）；比较一定要求分隔符边界，否则 "model" 会匹配上 "model2"。 */
#include "model_import_path.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

/* 分隔符等价与大小写折叠只作用于比较，不修改调用方传入的字符串；这两条规则必须与
 * model_import_digest_path.c 里的同名判断保持一致（两边各自实现，改一处要改两处）。 */
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

/* 去掉尾部分隔符，再截到最后一个分隔符，并保留根（"/" 或 "C:\"）。
 * 返回 false 表示没有父目录或容量不足，此时不会写入 parent。 */
bool bongo_cat_import_parent_path(const char *path, char *parent,
    size_t capacity) {
    size_t length = path ? strlen(path) : 0;
    while (length && separator(path[length - 1])) length--;
    while (length && !separator(path[length - 1])) length--;
    while (length > 1 && separator(path[length - 1])) length--;
    if (!length || length >= capacity) return false;
    memcpy(parent, path, length);
    parent[length] = '\0';
    return true;
}

/* root 必须按前缀匹配 path 且边界是分隔符，否则返回 false；成功时把剩余部分写进 relative。
 * 容量不足返回 false，但 relative 里可能已经被 snprintf 截断，别拿去用。 */
bool bongo_cat_import_relative_path(const char *root, const char *path,
    char *relative, size_t capacity) {
    size_t root_length = root ? strlen(root) : 0;
    while (root_length > 1 && separator(root[root_length - 1])) root_length--;
    size_t path_length = path ? strlen(path) : 0;
    if (!root_length || path_length < root_length) return false;
    for (size_t i = 0; i < root_length; ++i)
        if (!character_equal(root[i], path[i])) return false;
    if (path_length > root_length && !separator(path[root_length]))
        return false;
    const char *value = path + root_length;
    while (separator(*value)) value++;
    int written = snprintf(relative, capacity, "%s", value);
    return written >= 0 && (size_t)written < capacity;
}

/* 后缀比较：空后缀视为不匹配（否则任何名字都算命中）；_ci 用 SDL_strcasecmp，不做编码归一。 */
bool bongo_cat_import_has_suffix(const char *value, const char *suffix) {
    size_t value_length = value ? strlen(value) : 0;
    size_t suffix_length = suffix ? strlen(suffix) : 0;
    return value_length >= suffix_length && suffix_length > 0 &&
        memcmp(value + value_length - suffix_length, suffix, suffix_length) == 0;
}

bool bongo_cat_import_has_suffix_ci(const char *value, const char *suffix) {
    size_t value_length = value ? strlen(value) : 0;
    size_t suffix_length = suffix ? strlen(suffix) : 0;
    return value_length >= suffix_length && suffix_length > 0 &&
        SDL_strcasecmp(value + value_length - suffix_length, suffix) == 0;
}
