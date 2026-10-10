/* 内容指纹的文件级记忆表：键是路径，有效性由（大小，修改时间）判定，值是该文件的 SHA-256。
 * 一次导入会话共用一个缓存（见 model_import_session），让同一份资源在不同候选、不同子指纹
 * 里重复出现时只哈希一次。
 * 无锁：约定每个导入会话一个缓存，只被持有该会话的那个线程访问。 */
#include "model_import.h"
#include "bongo_cat/sha256.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

/* 4093 个桶配链地址法，桶数固定不扩容，所以条目数上限单独设成 16384；到顶后 cache_store
 * 变成空操作 —— 只影响再次导入的速度，不影响 digest 的正确性。 */
#define DIGEST_CACHE_BUCKET_CAP 4093
#define DIGEST_CACHE_ENTRY_CAP 16384

/* path 是堆上的独立拷贝（本模块自己 malloc / free），digest 固定 65 字节含结尾 NUL；
 * 条目挂在桶的链表头部，有效性靠 size 与 modified 双条件守住。 */
typedef struct DigestCacheEntry {
    struct DigestCacheEntry *next;
    char *path;
    char digest[65];
    uint64_t size;
    uint64_t modified;
} DigestCacheEntry;

struct BongoCatImportDigestCache {
    DigestCacheEntry **buckets;
    size_t count;
};

/* FNV-1a：反斜杠归一成斜杠，Windows 上再折叠大小写，与 path_equal 的相等规则一一对应；
 * 只要求“相等的路径落在同一个桶”，不要求与最终 digest 有任何关系。 */
static uint64_t hash_path(const char *value) {
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char *cursor = (const unsigned char *)value;
        cursor && *cursor; ++cursor) {
        unsigned char byte = *cursor == '\\' ? '/' : *cursor;
#ifdef _WIN32
        if (byte >= 'A' && byte <= 'Z') byte = (unsigned char)(byte + 32);
#endif
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

static bool path_equal(const char *left, const char *right) {
#ifdef _WIN32
    return SDL_strcasecmp(left, right) == 0;
#else
    return strcmp(left, right) == 0;
#endif
}

/* 查找条目并返回借用指针（NULL 表示未命中）；bucket 非空时回传桶号，让 cache_store 插入时
 * 不必再算一次哈希。cache 为 NULL 是正常输入 —— 无缓存调用就是靠这里退化成未命中。 */
static DigestCacheEntry *cache_entry(BongoCatImportDigestCache *cache,
    const char *path, size_t *bucket) {
    if (!cache || !cache->buckets || !path) return NULL;
    size_t index = (size_t)(hash_path(path) % DIGEST_CACHE_BUCKET_CAP);
    if (bucket) *bucket = index;
    for (DigestCacheEntry *entry = cache->buckets[index]; entry;
        entry = entry->next)
        if (path_equal(entry->path, path)) return entry;
    return NULL;
}

/* 写入或刷新条目；分配失败或达到条目上限时静默跳过，所以这个缓存永远只是加速手段，
 * 不能当成“这个文件已经哈希过”的凭据。 */
static void cache_store(BongoCatImportDigestCache *cache, const char *path,
    uint64_t size, uint64_t modified, const char digest[65]) {
    if (!cache || !cache->buckets || cache->count >= DIGEST_CACHE_ENTRY_CAP)
        return;
    size_t bucket = 0;
    DigestCacheEntry *entry = cache_entry(cache, path, &bucket);
    if (!entry) {
        entry = calloc(1, sizeof(*entry));
        size_t length = strlen(path) + 1;
        if (entry) entry->path = malloc(length);
        if (!entry || !entry->path) {
            if (entry) free(entry->path);
            free(entry);
            return;
        }
        memcpy(entry->path, path, length);
        entry->next = cache->buckets[bucket];
        cache->buckets[bucket] = entry;
        cache->count++;
    }
    entry->size = size;
    entry->modified = modified;
    memcpy(entry->digest, digest, 65);
}

/* 创建空缓存（桶一次分配好，之后不再扩容）。返回 NULL 表示内存不足，调用方要把它当成
 * 可选参数处理；所有权归调用方，用 bongo_cat_import_digest_cache_destroy 释放。 */
BongoCatImportDigestCache *bongo_cat_import_digest_cache_create(void) {
    BongoCatImportDigestCache *cache = calloc(1, sizeof(*cache));
    if (cache) cache->buckets = calloc(DIGEST_CACHE_BUCKET_CAP,
        sizeof(*cache->buckets));
    if (cache && cache->buckets) return cache;
    if (cache) free(cache->buckets);
    free(cache);
    return NULL;
}

/* 释放所有条目与缓存本身，接受 NULL。调用前必须确认没有别的线程还在用它
 * （本类型自身不加锁）。 */
void bongo_cat_import_digest_cache_destroy(BongoCatImportDigestCache *cache) {
    if (!cache) return;
    if (cache->buckets) for (size_t i = 0;
        i < DIGEST_CACHE_BUCKET_CAP; ++i) {
        DigestCacheEntry *entry = cache->buckets[i];
        while (entry) {
            DigestCacheEntry *next = entry->next;
            free(entry->path);
            free(entry);
            entry = next;
        }
    }
    free(cache->buckets);
    free(cache);
}

/* 取文件的内容哈希：命中且 size 与 modified 都相同就复用，否则重算 SHA-256 并回填。
 * 返回 false 表示读文件失败（output 内容未定义，调用方不要使用）；cache 为 NULL 时退化为
 * 每次都算，结果依旧正确。 */
bool bongo_cat_import_digest_file_cached(BongoCatImportDigestCache *cache,
    const char *path, uint64_t size, uint64_t modified, char output[65]) {
    DigestCacheEntry *entry = cache_entry(cache, path, NULL);
    if (entry && entry->size == size && entry->modified == modified) {
        memcpy(output, entry->digest, 65);
        return true;
    }
    if (bongo_cat_sha256_file(path, output, NULL) != BONGO_CAT_OK)
        return false;
    cache_store(cache, path, size, modified, output);
    return true;
}
