/* 导入模型的内容身份（content digest）：遍历候选包的全部资源根，对每个文件取按
 * （大小，修改时间）记忆化的 SHA-256，再把文件集合聚合成指纹，最后对一行固定格式的摘要做
 * SHA-256，得到 65 字节十六进制 id。时序上位于“发现候选”之后、“安装”之前，调用方是
 * prepare_package_metadata_cached（安装、索引校验）与 nearby 扫描（经无缓存入口）。
 * 契约：同一份文件树无论在哪个绝对路径下都要得到同一 digest，所以哈希输入一律相对根计算；
 * 反过来，改动根分组、聚合方式或摘要格式等于让已安装模型的身份全部失效；最易改错的地方是
 * add_root 的调用顺序与合并规则，以及递归与容量上限（超限一律失败，不做截断）。 */
#include "model_import.h"
#include "model_import_digest_internal.h"
#include "model_import_mver_policy.h"
#include "bongo_cat/path.h"
#include "bongo_cat/sha256.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* 根数量上限：实际根数由候选字段决定（模型目录、resources、封面/背景、config，个位数），
 * 12 只是余量；真溢出时 add_root 会静默丢弃新根，不会报错。 */
#define DIGEST_ROOT_CAP 12
#define path_length bongo_cat_import_digest_path_length
#define path_contains bongo_cat_import_digest_path_contains
#define hash_text bongo_cat_import_digest_hash_text
#define mix bongo_cat_import_digest_mix

/* 一个根或子树的内容指纹。sum 用加法、exclusive 用异或聚合，两者都与顺序无关且互相补充，
 * 让“文件集合不同但字节数相同”的包不会撞成同一个 digest；四个量最后都进入摘要。 */
typedef struct DigestStamp { uint64_t sum, exclusive, bytes, files, entries; } DigestStamp;

typedef struct DigestWalk {
    const char *root;
    const char *group;
    DigestStamp *stamp;
    const char *content_root;
    DigestStamp *content;
    const char *override_root;
    DigestStamp *overrides;
    BongoCatImportDigestCache *cache;
    int depth;
} DigestWalk;

typedef struct DigestRoot { char path[BONGO_CAT_PATH_CAP]; const char *group; } DigestRoot;

static bool stamp_path(const char *path, DigestWalk *walk);

/* 枚举回调：拼路径并递归，条目超过 16384 个即返回 FAILURE —— 上限必须是硬失败，因为指纹
 * 漏掉一个文件就可能把两个不同的包判成同一个；CONTINUE 继续遍历，FAILURE 终止整个枚举。 */
static BongoCatPathVisit stamp_item(void *userdata,
    const char *dirname, const char *name) {
    DigestWalk *walk = userdata;
    char path[BONGO_CAT_PATH_CAP];
    return ++walk->stamp->entries <= 16384 &&
        bongo_cat_path_join(path, sizeof(path), dirname, name) &&
        stamp_path(path, walk) ? BONGO_CAT_PATH_CONTINUE :
        BONGO_CAT_PATH_FAILURE;
}

/* 相对根的路径才会进哈希：同一个包换个目录导入必须得到同一个身份。
 * path 不在 root 之下时原样返回，这是防御性兜底，正常调用路径不该发生。 */
static const char *relative_path(const char *root, const char *path) {
    size_t root_length = path_length(root);
    if (!path_contains(root, path)) return path;
    const char *relative = path + root_length;
    while (*relative == '/' || *relative == '\\') relative++;
    return relative;
}

/* 递归主体：目录交给 enumerate 继续走，文件先取（大小，修改时间）再查缓存哈希。
 * 深度 24、文件数 8192、累计 1 GiB 都是硬上限；任何越界或 IO 失败都返回 false，由调用方
 * 转成“无法指纹化”的错误 —— 宁可让导入失败，也不要生成一份覆盖不全的假身份。 */
static bool stamp_path(const char *path, DigestWalk *walk) {
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
    char file_hash[65];
    if (!bongo_cat_import_digest_file_cached(walk->cache, path, size,
        modified, file_hash))
        return false;
    uint64_t value = mix(hash_text(walk->group) ^
        hash_text(relative_path(walk->root, path)) ^ hash_text(file_hash) ^
        mix(size));
    walk->stamp->sum += value;
    walk->stamp->exclusive ^= value;
    walk->stamp->bytes += size;
    walk->stamp->files++;
    /* 同一个文件还会按“是否落在候选的 assets / overrides 之下”再累计到两个子指纹，供占位
     * 模板判定使用；子指纹的路径相对各自子根，标签固定为 content / overrides。 */
    if (walk->content && path_contains(walk->content_root, path)) {
        value = mix(hash_text("content") ^
            hash_text(relative_path(walk->content_root, path)) ^
            hash_text(file_hash) ^ mix(size));
        walk->content->sum += value;
        walk->content->exclusive ^= value;
        walk->content->bytes += size;
        walk->content->files++;
    }
    if (walk->overrides && path_contains(walk->override_root, path)) {
        value = mix(hash_text("overrides") ^
            hash_text(relative_path(walk->override_root, path)) ^
            hash_text(file_hash) ^ mix(size));
        walk->overrides->sum += value;
        walk->overrides->exclusive ^= value;
        walk->overrides->bytes += size;
        walk->overrides->files++;
    }
    return true;
}

/* 遍历单个根；root 为空表示候选没有这个字段（例如 Tauri 的 overrides），视为零内容成功。 */
static bool stamp_root(const char *root, const char *group,
    DigestStamp *stamp, const BongoCatImportCandidate *candidate,
    DigestStamp *content, DigestStamp *overrides,
    BongoCatImportDigestCache *cache) {
    if (!root || !root[0]) return true;
    DigestWalk walk = {root, group, stamp,
        candidate->assets, content, candidate->overrides, overrides, cache, 0};
    return stamp_path(root, &walk);
}

/* 注册一个哈希根，并维持“根之间互不包含”的不变量：已被更外层根覆盖的路径直接跳过；
 * 反过来，新根若覆盖了已登记的根，那些根会被移除，新根的标签改成 content。
 * 标签进了每个文件的哈希，所以这里的调用顺序是身份的一部分，调整顺序会改变 digest。 */
static void add_root(DigestRoot roots[DIGEST_ROOT_CAP], size_t *count,
    const char *path, const char *group) {
    if (!path || !path[0]) return;
    for (size_t i = 0; i < *count; ++i)
        if (path_contains(roots[i].path, path)) return;
    bool merged = false;
    for (size_t i = 0; i < *count;) {
        if (!path_contains(path, roots[i].path)) { i++; continue; }
        roots[i] = roots[--*count];
        merged = true;
    }
    if (merged) group = "content";
    if (*count < DIGEST_ROOT_CAP) {
        DigestRoot *root = &roots[(*count)++];
        snprintf(root->path, sizeof(root->path), "%s", path);
        root->group = group;
    }
}

/* 只为判断 config 是否已被某个根覆盖，避免把模型目录里的那份 manifest 重复计入。 */
static bool roots_contain(const DigestRoot *roots, size_t count,
    const char *path) {
    for (size_t i = 0; i < count; ++i)
        if (path_contains(roots[i].path, path)) return true;
    return false;
}

/* 按给定顺序返回第一个真实存在的文件，并回传命中的文件名（供 group 标签使用）。
 * 顺序即优先级，换顺序会改变 Tauri 预览根的标签与数量，进而改变 digest。 */
static bool first_file(const char *directory, const char *const *names,
    size_t name_count, char *path, size_t capacity, const char **name) {
    for (size_t i = 0; i < name_count; ++i)
        if (bongo_cat_path_join(path, capacity, directory, names[i]) &&
            bongo_cat_path_is_file(path)) {
            if (name) *name = names[i];
            return true;
        }
    return false;
}

/* Tauri 皮肤的预览来源：优先打包好的 resources/，其次是 assets/ 下的同名文件，另外单独认
 * 一张手写的 cover.png。选中的封面/背景各自成根，所以只换一张预览图就是另一个皮肤；已经
 * 落在 resources/ 里的那份被 resources 根覆盖，不再重复添加。 */
static void add_tauri_preview_roots(const BongoCatImportCandidate *candidate,
    DigestRoot roots[DIGEST_ROOT_CAP], size_t *count) {
    static const char *const covers[] = {
        "cover.png", "cat.png", "bg.png", "mousebg.png", "tabletbg.png"
    };
    static const char *const backgrounds[] = {
        "background.png", "bg.png", "mousebg.png", "tabletbg.png"
    };
    char resources[BONGO_CAT_PATH_CAP], path[BONGO_CAT_PATH_CAP];
    bool has_resources = bongo_cat_path_join(resources, sizeof(resources),
        candidate->assets, "resources") && bongo_cat_path_is_dir(resources);
    if (has_resources)
        add_root(roots, count, resources, "preview-resources");
    const char *name = NULL;
    bool resource_cover = has_resources && first_file(resources, covers,
        sizeof(covers) / sizeof(covers[0]), path, sizeof(path), NULL);
    if (!resource_cover && first_file(candidate->assets, covers,
            sizeof(covers) / sizeof(covers[0]), path, sizeof(path), &name))
        add_root(roots, count, path, name);
    bool resource_background = has_resources && first_file(resources,
        backgrounds, sizeof(backgrounds) / sizeof(backgrounds[0]), path,
        sizeof(path), NULL);
    if (!resource_background && first_file(candidate->assets, backgrounds,
            sizeof(backgrounds) / sizeof(backgrounds[0]), path,
            sizeof(path), &name))
        add_root(roots, count, path, name);
    if (bongo_cat_path_join(path, sizeof(path), candidate->assets,
            "cover.png") && bongo_cat_path_is_file(path))
        add_root(roots, count, path, "authored-cover");
}

static bool stamp_equal(const DigestStamp *value, uint64_t sum, uint64_t exclusive,
    uint64_t bytes, uint64_t files) {
    return value->sum == sum && value->exclusive == exclusive &&
        value->bytes == bytes && value->files == files;
}

/* 判定候选是否为随包运行时模板（占位内容），返回 true 表示调用方应当丢弃它。
 * Tauri 永远不是；MVER 看渲染核心（mver_stock_model）；MVER_PATCH 另看覆盖集指纹 ——
 * 下面那几个常量就是随包 gamepad 覆盖集的指纹，重新导出模板后必须同步，否则识别会失效。 */
static bool candidate_placeholder(const BongoCatImportCandidate *candidate,
    const DigestStamp *overrides,
    BongoCatImportDigestCache *cache) {
    /* Identify bundled runtime templates by their rendered Live2D core. Do
       not use aggregate asset stamps here: older releases shipped a popular
       authored skin with the same file counts and byte totals. */
    if (candidate->format == BONGO_CAT_IMPORT_TAURI) return false;
    if (bongo_cat_import_mver_stock_model(candidate, cache)) return true;
    if (candidate->format == BONGO_CAT_IMPORT_MVER_PATCH) {
        if (!overrides->files) return true;
        if (candidate->mode != BONGO_CAT_MODE_GAMEPAD) return false;
        return stamp_equal(overrides, 0x3830b3ef1cf413b7ull,
                0xf36e3216ab32c519ull, 828360ull, 12ull) ||
            stamp_equal(overrides, 0xe35c6e634128d62bull,
                0x17e2f6d0ad5746bfull, 1297880ull, 12ull);
    }
    return false;
}

/* 内容指纹主入口。前提：candidate 的路径字段已由 discover 填好，candidate->setting 是目录
 * 下真实存在的 manifest 文件名（否则报 FORMAT 错误）。
 * 成功时 output 写入 65 字节 NUL 结尾的十六进制 digest，placeholder（非 NULL 时）表示
 * “这是随包模板”；cache 可为 NULL，那时每个文件都重新哈希。失败返回 false 并尽量写
 * error —— 参数为空与摘要溢出这两条路径不写 error，调用方必须容忍。 */
bool bongo_cat_import_candidate_inspect_cached(
    const BongoCatImportCandidate *candidate, char output[65],
    bool *placeholder, BongoCatImportDigestCache *cache,
    BongoCatError *error) {
    if (!candidate || !output) return false;
    char setting[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(setting, sizeof(setting),
            candidate->directory, candidate->setting) ||
        !bongo_cat_path_is_file(setting)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            "Imported model manifest is missing: %s", candidate->setting);
        return false;
    }
    DigestRoot roots[DIGEST_ROOT_CAP] = {0};
    size_t root_count = 0;
    add_root(roots, &root_count, candidate->directory, "model");
    if (candidate->format == BONGO_CAT_IMPORT_TAURI)
        add_tauri_preview_roots(candidate, roots, &root_count);
    else {
        add_root(roots, &root_count, candidate->assets, "assets");
        add_root(roots, &root_count, candidate->overrides, "overrides");
    }
    if (candidate->config[0] &&
        !roots_contain(roots, root_count, candidate->config))
        add_root(roots, &root_count, candidate->config, "configuration");
    DigestStamp stamp = {0}, content = {0}, overrides = {0};
    for (size_t i = 0; i < root_count; ++i) {
        if (stamp_root(roots[i].path, roots[i].group, &stamp, candidate,
                &content, &overrides, cache)) continue;
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Cannot fingerprint imported model assets: %s",
            candidate->package_root);
        return false;
    }
    /* 这一行是 digest 的稳定序列化格式：字段、顺序、宽度都要当接口看待，
     * 改动它会让已安装模型的对账去重全部失效。 */
    char summary[512];
    int length = snprintf(summary, sizeof(summary),
        "%016llx|%s|%d|%d|%016llx|%016llx|%llu|%llu",
        (unsigned long long)hash_text(candidate->setting),
        bongo_cat_mode_name(candidate->mode), (int)candidate->format,
        candidate->gamepad_buttons ? 1 : 0,
        (unsigned long long)stamp.sum,
        (unsigned long long)stamp.exclusive,
        (unsigned long long)stamp.bytes,
        (unsigned long long)stamp.files);
    if (length < 0 || (size_t)length >= sizeof(summary)) return false;
    bongo_cat_sha256_bytes(summary, (size_t)length, output);
    if (placeholder)
        *placeholder = candidate_placeholder(candidate, &overrides, cache);
    return true;
}
