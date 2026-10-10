/*
 * Mver 的"这是不是内置模型"策略: 用清单里 Moc 与 Textures 的内容指纹 (而不是整个目录的聚合指纹)
 * 比对内置 runtime 模型的已知档位; 图片补丁则先剥掉覆盖层, 再对底层包做同样的判断。判定为内置
 * 模板的候选不会作为用户模型出现在目录里 (由 candidate_placeholder 调用)。
 * 契约: 三个导出函数都不修改 candidate; 指纹是 65 字节的十六进制 sha256, 由调用方的
 * BongoCatImportDigestCache 加速。
 * 易错点: profiles 是硬编码指纹, 内置模型一变就必须重新生成, 否则新内置模型会以用户模型变体的身份冒出来。
 */
#include "model_import_mver_policy.h"
#include "model_import_mver_manifest.h"
#include "bongo_cat/file.h"
#include "bongo_cat/path.h"

#include <stdio.h>
#include <string.h>
#include <yyjson.h>

typedef struct StockLive2DProfile {
    BongoCatModelMode mode;
    const char *moc;
    const char *textures[3];
    size_t texture_count;
} StockLive2DProfile;

/* 把补丁候选还原成底层完整包再取指纹: 复制候选, 强制 format 为 MVER 并清空 patch_root 与
   overrides, 算出来的才是模型本体的 identity, 用于判断补丁是不是只在改内置模板。candidate 不是
   补丁时直接返回 false。 */
bool bongo_cat_import_patch_base_inspect(
    const BongoCatImportCandidate *candidate, char output[65],
    bool *placeholder, BongoCatError *error) {
    if (!candidate || candidate->format != BONGO_CAT_IMPORT_MVER_PATCH)
        return false;
    BongoCatImportCandidate base = *candidate;
    base.format = BONGO_CAT_IMPORT_MVER;
    base.patch_root[0] = '\0';
    base.overrides[0] = '\0';
    return bongo_cat_import_candidate_inspect(&base, output, placeholder, error);
}

/* discovery 里是否已经存在这个补丁所依附的完整 Mver 候选 (模式、目录与清单文件全同)。调用方据此
   跳过补丁候选让完整包胜出, 避免同一个模型列两次; 下标越界返回 false。 */
bool bongo_cat_import_patch_has_full_base(
    const BongoCatImportDiscovery *discovery, size_t candidate_index) {
    if (!discovery || candidate_index >= discovery->count) return false;
    const BongoCatImportCandidate *patch =
        &discovery->candidates[candidate_index];
    if (patch->format != BONGO_CAT_IMPORT_MVER_PATCH) return false;
    for (size_t i = 0; i < discovery->count; ++i) {
        const BongoCatImportCandidate *base = &discovery->candidates[i];
        if (base->format == BONGO_CAT_IMPORT_MVER &&
            base->mode == patch->mode &&
            strcmp(base->directory, patch->directory) == 0 &&
            strcmp(base->setting, patch->setting) == 0) return true;
    }
    return false;
}

/* 校验清单里的相对引用: 拒绝空串、以斜杠开头的路径、含冒号的盘符或流名, 以及任何一个只由两个点
   组成的路径分量。这是拼接清单内容时的目录穿越防线, 放宽它会允许模型读走包外的文件。 */
static bool safe_model_reference(const char *value) {
    if (!value || !value[0] || value[0] == '/' || value[0] == '\\' ||
        strchr(value, ':')) return false;
    const char *part = value;
    while (*part) {
        while (*part == '/' || *part == '\\') part++;
        if (part[0] == '.' && part[1] == '.' &&
            (!part[2] || part[2] == '/' || part[2] == '\\')) return false;
        part = strpbrk(part, "/\\");
        if (!part) break;
    }
    return true;
}

/* 拼路径并取缓存指纹; 引用非法或文件不存在都返回 false (不是错误), 由调用方定夺。 */
static bool reference_digest(const char *root, const char *relative,
    BongoCatImportDigestCache *cache, char digest[65]) {
    char path[BONGO_CAT_PATH_CAP];
    uint64_t size = 0, modified = 0;
    return safe_model_reference(relative) &&
        bongo_cat_path_join(path, sizeof(path), root, relative) &&
        bongo_cat_path_file_info(path, &size, &modified) &&
        bongo_cat_import_digest_file_cached(cache, path, size, modified, digest);
}

/* 档位比对: 模式必须相同, Textures 的个数必须与档位一致 (多一张少一张都算不同), 然后逐个比对
   Moc 与每张贴图的指纹。 */
static bool profile_matches(const StockLive2DProfile *profile,
    const BongoCatImportCandidate *candidate, yyjson_val *refs,
    BongoCatImportDigestCache *cache) {
    if (profile->mode != candidate->mode) return false;
    const char *moc = yyjson_get_str(yyjson_obj_get(refs, "Moc"));
    yyjson_val *textures = yyjson_obj_get(refs, "Textures");
    if (!yyjson_is_arr(textures) ||
        yyjson_arr_size(textures) != profile->texture_count) return false;
    char digest[65];
    if (!reference_digest(candidate->directory, moc, cache, digest) ||
        strcmp(digest, profile->moc) != 0) return false;
    for (size_t i = 0; i < profile->texture_count; ++i) {
        const char *texture = yyjson_get_str(yyjson_arr_get(textures, i));
        if (!reference_digest(candidate->directory, texture, cache, digest) ||
            strcmp(digest, profile->textures[i]) != 0) return false;
    }
    return true;
}

/* 判断候选是否就是内置 runtime 模型 (占位模板): 读它的清单, 拿 FileReferences 逐个与 profiles
   比对。返回 true 表示不该把它当用户模型; 读不到清单返回 false, 按普通模型处理。 */
bool bongo_cat_import_mver_stock_model(
    const BongoCatImportCandidate *candidate,
    BongoCatImportDigestCache *cache) {
    if (!candidate) return false;
    /* Mver skins commonly retain these runtime models while replacing only
       mode input images. Match the model's rendered core independently from
       those surrounding images so the bundled model cannot become a variant. */
    static const StockLive2DProfile profiles[] = {
        {BONGO_CAT_MODE_STANDARD,
            "7bbcdb3df4fe085b0cbd9dc3a1cf32d351bd56787d0ddd1c238e50a5dcb6729a",
            {"f20955f2779f52dddefae131e6c7c5506ce4261d60cf020d594e095e5a2193b2",
             "bf32b89f328160ac8bd7cd4e527646eedfae98294506c597f381001ae6a83eb4",
             "93c3213355e4743b38e8dd2e14a24693f2d24f10dc8b51c4ca986a749ee24682"}, 3},
        {BONGO_CAT_MODE_KEYBOARD,
            "03ed67f3ee2ea612aba4da0d42874f8879853d69043c9aae98af440d1f66965e",
            {"3153891ec9c6b89ce9b33a610fb081d276ff356c5e19407d5ff0690d7ac7d97a",
             "3332efb5349cdf54ea70f1ba491791d3690d48876d8abea19cb7c3f3ef383d93",
             "681fcc7634d723cb10cfe4efa4eccb16c2b90d51647fff2f9904aaa298f030c5"}, 3},
        {BONGO_CAT_MODE_GAMEPAD,
            "e7f11d627011bb2c65d8b0882ce4545115d2256672dca256b674a713e3e5f3d6",
            {"9e593239d867b90966b0f3df81ac98aad7e74eb35cba81f3cf961dfb9bf495ea",
             "3332efb5349cdf54ea70f1ba491791d3690d48876d8abea19cb7c3f3ef383d93",
             "681fcc7634d723cb10cfe4efa4eccb16c2b90d51647fff2f9904aaa298f030c5"}, 3},
        {BONGO_CAT_MODE_KEYBOARD,
            "03ed67f3ee2ea612aba4da0d42874f8879853d69043c9aae98af440d1f66965e",
            {"0a17e9c74b0269e0d8c4291da03825fe81d20c09da520537127140fbd0900400",
             "bf32b89f328160ac8bd7cd4e527646eedfae98294506c597f381001ae6a83eb4",
             "93c3213355e4743b38e8dd2e14a24693f2d24f10dc8b51c4ca986a749ee24682"}, 3},
        {BONGO_CAT_MODE_GAMEPAD,
            "e7f11d627011bb2c65d8b0882ce4545115d2256672dca256b674a713e3e5f3d6",
            {"8238890702166583db122e9d4f0a3c3d1717e3188ab8bfd4a264a2a4e13a053d",
             "bf32b89f328160ac8bd7cd4e527646eedfae98294506c597f381001ae6a83eb4",
             "93c3213355e4743b38e8dd2e14a24693f2d24f10dc8b51c4ca986a749ee24682"}, 3}
    };
    /* 同一模式出现多个档位是不同发行版的内置贴图组合, 顺序无关; 新增或更新内置模型时把新的 Moc
       与 Textures 指纹追加到这里。 */
    char manifest[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(manifest, sizeof(manifest), candidate->directory,
            candidate->setting)) return false;
    yyjson_doc *document = bongo_cat_import_mver_manifest_read(manifest, NULL);
    yyjson_val *root = document ? yyjson_doc_get_root(document) : NULL;
    yyjson_val *refs = yyjson_is_obj(root)
        ? yyjson_obj_get(root, "FileReferences") : NULL;
    bool matched = false;
    for (size_t i = 0; yyjson_is_obj(refs) &&
        i < sizeof(profiles) / sizeof(profiles[0]); ++i)
        if (profile_matches(&profiles[i], candidate, refs, cache)) {
            matched = true;
            break;
        }
    yyjson_doc_free(document);
    return matched;
}
