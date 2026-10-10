/*
 * 就近模型的来源签名: 把候选的所有来源根 (模型目录、assets、overrides) 连同配置文件的 sha256 压成
 * 一个 64 位十六进制串, 用来判断这个目录自上次扫描后有没有变过。
 * 位置: nearby 扫描在查缓存与写缓存时各调一次, 全程在 model_import_lock 之内。
 * 契约: 不读文件内容, 只用大小与修改时间 —— 快, 但同尺寸同时间戳的改写检测不到; 它只用来决定要
 * 不要重新指纹化, 真正的 identity 仍由内容指纹给出。
 * 易错点: 根的顺序与分组名都参与哈希, 改算法等于让全部旧缓存失效。
 */
#include "model_import_nearby_internal.h"
#include "bongo_cat/path.h"
#include "bongo_cat/sha256.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SIGNATURE_ROOT_CAP 3

/* 一个来源根的累计量: 每个文件的混合值同时加进 sum 与异或进 exclusive, 另记总字节、最新修改时间
   与文件数; 这几个量都要进最终摘要, 只看其中一个都可能漏掉变化。 */
typedef struct SignatureStamp {
    uint64_t sum, exclusive, bytes, latest, files, entries;
} SignatureStamp;

typedef struct SignatureRoot {
    char path[BONGO_CAT_PATH_CAP];
    const char *group;
} SignatureRoot;

typedef struct SignatureWalk {
    const SignatureRoot *root;
    SignatureStamp *stamp;
    int depth;
} SignatureWalk;

/* 路径比较按可移植语义做: 两种斜杠等价, Windows 上忽略大小写。只用于判断包含关系与算相对路径,
   不用于访问文件系统。 */
static bool separator(char value) { return value == '/' || value == '\\'; }

static bool character_equal(char left, char right) {
    if (separator(left) && separator(right)) return true;
#ifdef _WIN32
    if (left >= 'A' && left <= 'Z') left = (char)(left - 'A' + 'a');
    if (right >= 'A' && right <= 'Z') right = (char)(right - 'A' + 'a');
#endif
    return left == right;
}

static size_t normalized_length(const char *path) {
    size_t length = path ? strlen(path) : 0;
    while (length > 1 && separator(path[length - 1])) length--;
    return length;
}

/* path 是否位于 root 之内 (含相等)。末尾分隔符先归一化, 保证 a/b 不会匹配到 a/bc; 把绝对路径折算
   成相对形式是模型换目录后签名不变的前提。 */
static bool contains(const char *root, const char *path) {
    size_t root_length = normalized_length(root);
    size_t path_length = normalized_length(path);
    if (!root_length || path_length < root_length) return false;
    for (size_t i = 0; i < root_length; ++i)
        if (!character_equal(root[i], path[i])) return false;
    return path_length == root_length || separator(path[root_length]);
}

/* 登记一个来源根, 最多 SIGNATURE_ROOT_CAP 个。已被新根包含的旧根直接跳过; 反过来包含旧根的新根会
   移除那些旧根, 并改用 content 组名 —— 组名参与哈希, 不统一的话同一份内容会因为根的重叠方式不同
   而算出不同签名。 */
static void add_root(SignatureRoot roots[SIGNATURE_ROOT_CAP], size_t *count,
    const char *path, const char *group) {
    if (!path || !path[0]) return;
    for (size_t i = 0; i < *count; ++i)
        if (contains(roots[i].path, path)) return;
    bool merged = false;
    for (size_t i = 0; i < *count;) {
        if (!contains(path, roots[i].path)) { i++; continue; }
        roots[i] = roots[--*count];
        merged = true;
    }
    if (*count >= SIGNATURE_ROOT_CAP) return;
    SignatureRoot *root = &roots[(*count)++];
    snprintf(root->path, sizeof(root->path), "%s", path);
    root->group = merged ? "content" : group;
}

/* 64 位 FNV-1a; 反斜杠先归一成斜杠, 因此同一路径的两种写法同值。 */
static uint64_t text_hash(const char *value) {
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char *cursor = (const unsigned char *)value;
        cursor && *cursor; ++cursor) {
        unsigned char byte = *cursor == '\\' ? '/' : *cursor;
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

static uint64_t mix(uint64_t value) {
    value ^= value >> 30; value *= 0xbf58476d1ce4e5b9ull;
    value ^= value >> 27; value *= 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}

static bool stamp_path(const char *path, SignatureWalk *walk);

/* 目录 enumerator: 累计条目数与文件数, 超过任一枚举上限就返回 FAILURE 让整次签名计算失败 —— 与其
   给一个漏了尾巴的签名, 不如让调用方退回真实指纹。 */
static BongoCatPathVisit stamp_item(void *userdata,
    const char *dirname, const char *name) {
    SignatureWalk *walk = userdata;
    char path[BONGO_CAT_PATH_CAP];
    return ++walk->stamp->entries <= 16384 &&
        bongo_cat_path_join(path, sizeof(path), dirname, name) &&
        stamp_path(path, walk) ? BONGO_CAT_PATH_CONTINUE :
        BONGO_CAT_PATH_FAILURE;
}

static const char *relative_path(const SignatureRoot *root,
    const char *path) {
    if (!contains(root->path, path)) return path;
    const char *relative = path + normalized_length(root->path);
    while (separator(*relative)) relative++;
    return relative;
}

/* 递归累计一个路径: 目录限深 24 层, 累计字节上限 1 GiB, 超限一律失败。文件用组名、相对路径、大小
   与修改时间混合出 64 位值, 再加进 sum 与 exclusive 两个不同性质的累加器 (加法与异或), 比单用
   一个更难撞车。 */
static bool stamp_path(const char *path, SignatureWalk *walk) {
    if (bongo_cat_path_is_dir(path)) {
        if (walk->depth >= 24) return false;
        walk->depth++;
        bool ok = bongo_cat_path_enumerate(path, stamp_item, walk);
        walk->depth--;
        return ok;
    }
    uint64_t size = 0, modified = 0;
    if (!bongo_cat_path_file_info(path, &size, &modified) ||
        walk->stamp->files >= 8192 ||
        size > 1073741824ull - walk->stamp->bytes) return false;
    uint64_t value = mix(text_hash(walk->root->group) ^
        text_hash(relative_path(walk->root, path)) ^ mix(size) ^ mix(modified));
    walk->stamp->sum += value;
    walk->stamp->exclusive ^= value;
    walk->stamp->bytes += size;
    if (modified > walk->stamp->latest) walk->stamp->latest = modified;
    walk->stamp->files++;
    return true;
}

/* 计算候选的来源签名。config 存在时先取它的 sha256 参与摘要 (清单改动必须让签名变化); 任一根遍历
   失败都返回 false 并填 error, 这时调用方要把候选当作已变化处理, 不要继续沿用旧签名。输出固定
   65 字节 (64 位十六进制加结尾 0)。 */
bool bongo_cat_nearby_signature(const BongoCatImportCandidate *candidate,
    char output[65], BongoCatError *error) {
    if (!candidate || !output) return false;
    char config_hash[65] = {0};
    if (candidate->config[0] && bongo_cat_sha256_file(candidate->config,
        config_hash, error) != BONGO_CAT_OK) return false;
    SignatureRoot roots[SIGNATURE_ROOT_CAP] = {0};
    size_t root_count = 0;
    add_root(roots, &root_count, candidate->directory, "model");
    add_root(roots, &root_count, candidate->assets, "assets");
    add_root(roots, &root_count, candidate->overrides, "overrides");
    SignatureStamp stamp = {0};
    for (size_t i = 0; i < root_count; ++i) {
        SignatureWalk walk = {&roots[i], &stamp, 0};
        if (stamp_path(roots[i].path, &walk)) continue;
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Cannot inspect nearby model assets: %s",
            candidate->package_root);
        return false;
    }
    char summary[256];
    int length = snprintf(summary, sizeof(summary),
        "%s|%s|%d|%016llx|%016llx|%llu|%llu|%llu", config_hash,
        bongo_cat_mode_name(candidate->mode), (int)candidate->format,
        (unsigned long long)stamp.sum,
        (unsigned long long)stamp.exclusive,
        (unsigned long long)stamp.bytes,
        (unsigned long long)stamp.latest,
        (unsigned long long)stamp.files);
    if (length < 0 || (size_t)length >= sizeof(summary)) return false;
    bongo_cat_sha256_bytes(summary, (size_t)length, output);
    return true;
}
