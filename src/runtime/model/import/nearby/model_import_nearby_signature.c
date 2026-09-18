#include "model_import_nearby_internal.h"
#include "bongo_cat/path.h"
#include "bongo_cat/sha256.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SIGNATURE_ROOT_CAP 3

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

static bool contains(const char *root, const char *path) {
    size_t root_length = normalized_length(root);
    size_t path_length = normalized_length(path);
    if (!root_length || path_length < root_length) return false;
    for (size_t i = 0; i < root_length; ++i)
        if (!character_equal(root[i], path[i])) return false;
    return path_length == root_length || separator(path[root_length]);
}

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
