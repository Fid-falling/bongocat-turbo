#include "bongo_cat/update.h"

#include <stdio.h>
#include <string.h>
#include <yyjson.h>

/*
 * GitHub Releases 元数据的解析。
 *
 * 这里是唯一的信任边界: JSON 来自网络, 而其中的 URL 会被拿去下载甚至执行,
 * 所以文件名与 URL 前缀都要严格校验(见 safe_url / copy_asset_url): 只接受固定
 * 仓库的 releases 域名, 并要求资源名与拼出来的名字完全一致。
 * platform 串决定期望的资源名格式, 由调用方按当前构建传入; 命名规则同时被 CI
 * 的打包脚本依赖, 属于跨仓库契约, 改一边就要改另一边。
 *
 * 期望哈希 (update.h 的 BongoCatUpdateChecksum) 也在这里解析, 三级回退:
 *   1. 资源对象自己的 digest ("sha256:<hex>") —— GitHub 直接给的, 首选;
 *   2. release 说明里的 "SHA256 (<资源名>) = <hex>" 行;
 *   3. 同一个 release 里的 <资源名>.sha256 资产, 只抄地址, 内容由运行时**从官方源**抓。
 * 三级都拿不到时 checksum 保持 NONE, 运行时据此拒绝下载/替换 —— 绝不装没校验过的二进制。
 */
#define RELEASE_PREFIX \
    "https://github.com/Fid-falling/bongocat-turbo/releases/tag/"
#define DOWNLOAD_PREFIX \
    "https://github.com/Fid-falling/bongocat-turbo/releases/download/"

/* 统一的字符串字段拷贝。required 表示字段必须有且非空; truncate 表示超长时
   截断而不是失败 —— 只有 release notes 用它, 正文太长不该让整次更新检查失败。 */
static bool copy_text(char *target, size_t capacity, yyjson_val *value,
    bool required, bool truncate) {
    if (!value || yyjson_is_null(value)) return !required;
    if (!yyjson_is_str(value)) return false;
    const char *text = yyjson_get_str(value);
    size_t length = yyjson_get_len(value);
    if (!text || strlen(text) != length || (required && !length)) return false;
    if (length >= capacity) {
        if (!truncate) return false;
        length = capacity - 1;
    }
    memcpy(target, text, length);
    target[length] = '\0';
    return true;
}

/* 必须落在预期前缀(写死的 https://github.com/Fid-falling/bongocat-turbo/...)之下,
   且不能含控制字符或反斜杠: 前者防伪造域名, 后者防止 URL 在 Windows 上被当成
   本地路径解析。仓库改名后这里必须跟着 RELEASE_PREFIX/DOWNLOAD_PREFIX 一起改 ——
   GitHub API 返回的 html_url 用的是改名后的规范名, 前缀对不上会让所有更新检查
   直接判为非法元数据。 */
static bool safe_url(const char *url, const char *prefix) {
    if (!url || strncmp(url, prefix, strlen(prefix)) != 0) return false;
    for (const unsigned char *at = (const unsigned char *)url; *at; ++at)
        if (*at <= 0x20 || *at == '\\') return false;
    return true;
}

/* 资源的 name 与 browser_download_url 的最后一段都必须正好等于期望的文件名:
   只看 name 的话, URL 可能指向另一个文件。 */
static bool copy_asset_url(yyjson_val *asset, const char *expected,
    char *target, size_t capacity) {
    yyjson_val *name_value = yyjson_obj_get(asset, "name");
    yyjson_val *url_value = yyjson_obj_get(asset, "browser_download_url");
    if (!yyjson_is_str(name_value) || !yyjson_is_str(url_value) ||
        strcmp(yyjson_get_str(name_value), expected) != 0) return false;
    const char *url = yyjson_get_str(url_value);
    const char *filename = url ? strrchr(url, '/') : NULL;
    if (!safe_url(url, DOWNLOAD_PREFIX) || !filename ||
        strcmp(filename + 1, expected) != 0 || strlen(url) >= capacity)
        return false;
    snprintf(target, capacity, "%s", url);
    return true;
}

/* 在 assets 里找名字正好等于 expected 的资源并抄下文件名/下载地址; 顺手取它的 digest
   (与资源在同一个 JSON 对象里, 不用再遍历一次)。找不到返回 false, 此时 out 不变。 */
static bool find_asset(yyjson_val *assets, const char *expected,
    BongoCatUpdateAsset *out) {
    size_t index, count;
    yyjson_val *asset;
    yyjson_arr_foreach(assets, index, count, asset) {
        if (!yyjson_is_obj(asset)) continue;
        if (!copy_asset_url(asset, expected, out->url, sizeof(out->url)))
            continue;
        snprintf(out->name, sizeof(out->name), "%s", expected);
        yyjson_val *digest = yyjson_obj_get(asset, "digest");
        if (yyjson_is_str(digest))
            bongo_cat_update_sha256_text(yyjson_get_str(digest), out->sha256);
        return true;
    }
    return false;
}

/*
 * 期望哈希的三级回退 (顺序即信任优先级, 见文件头)。
 * 只在前一级拿不到时才看下一级: digest 存在就不再理会说明, 免得"说明写错了"把好端端的
 * digest 覆盖掉。第三级只抄地址, 不在检查阶段抓取 —— 检查更新不该额外联网。
 */
static void resolve_checksum(yyjson_val *assets, const char *notes,
    BongoCatUpdateAsset *out) {
    if (!out->name[0]) return;
    if (out->sha256[0]) {
        out->checksum = BONGO_CAT_UPDATE_CHECKSUM_DIGEST;
        return;
    }
    if (notes && bongo_cat_update_notes_sha256(notes, out->name, out->sha256)) {
        out->checksum = BONGO_CAT_UPDATE_CHECKSUM_NOTES;
        return;
    }
    char checksum_name[BONGO_CAT_UPDATE_ASSET_CAP + 16];
    int written = snprintf(checksum_name, sizeof(checksum_name), "%s.sha256",
        out->name);
    if (written < 0 || (size_t)written >= sizeof(checksum_name)) return;
    BongoCatUpdateAsset checksum_asset;
    memset(&checksum_asset, 0, sizeof(checksum_asset));
    if (find_asset(assets, checksum_name, &checksum_asset) &&
        checksum_asset.url[0]) {
        snprintf(out->checksum_url, sizeof(out->checksum_url), "%s",
            checksum_asset.url);
        out->checksum = BONGO_CAT_UPDATE_CHECKSUM_ASSET;
    }
}

/* 按平台拼出期望的安装包 / 便携包文件名。找不到对应资源不算错误, 只是对应字段
   保持为空, 由 UI 决定提示哪一种更新方式。
   便携包还有一个备选名 (BongoCat-<版本>-<平台>.exe): 万一发布流程以后只产出单文件 exe,
   老客户端仍然能匹配上; 主名存在时永远优先主名。 */
static bool read_assets(yyjson_val *root, const char *platform,
    BongoCatUpdateRelease *release) {
    yyjson_val *assets = yyjson_obj_get(root, "assets");
    if (!yyjson_is_arr(assets)) return false;
    char installer[128] = {0}, portable[128] = {0}, portable_alt[128] = {0};
    int lengths[3] = {0, 0, 0};
    if (strncmp(platform, "windows-", 8) == 0) {
        lengths[0] = snprintf(installer, sizeof(installer),
            "BongoCat-%s-%s-setup.exe", release->version, platform);
        lengths[1] = snprintf(portable, sizeof(portable),
            "BongoCat-%s-%s-portable.exe", release->version, platform);
        lengths[2] = snprintf(portable_alt, sizeof(portable_alt),
            "BongoCat-%s-%s.exe", release->version, platform);
    } else if (strcmp(platform, "linux-x64") == 0) {
        lengths[1] = snprintf(portable, sizeof(portable),
            "BongoCat-%s-%s.tar.gz", release->version, platform);
    } else if (strcmp(platform, "macos-x64") == 0 ||
        strcmp(platform, "macos-arm64") == 0) {
        lengths[1] = snprintf(portable, sizeof(portable),
            "BongoCat-%s-%s.zip", release->version, platform);
    } else return false;
    if (lengths[0] < 0 || (size_t)lengths[0] >= sizeof(installer) ||
        lengths[1] < 0 || (size_t)lengths[1] >= sizeof(portable) ||
        lengths[2] < 0 || (size_t)lengths[2] >= sizeof(portable_alt))
        return false;
    if (installer[0]) find_asset(assets, installer, &release->installer);
    if (portable[0] && !find_asset(assets, portable, &release->portable) &&
        portable_alt[0]) find_asset(assets, portable_alt, &release->portable);
    resolve_checksum(assets, release->notes, &release->installer);
    resolve_checksum(assets, release->notes, &release->portable);
    return true;
}

/* 草稿和预发布一律不认: 它们不是给普通用户装的。tag_name 允许带 v 前缀, 但存进
   release->version 的是去掉前缀并且能通过版本校验的那个字符串。 */
bool bongo_cat_update_parse_release(const char *json, const char *platform,
    BongoCatUpdateRelease *release, BongoCatError *error) {
    if (!json || !platform || !release) return false;
    memset(release, 0, sizeof(*release));
    yyjson_doc *document = yyjson_read(json, strlen(json), 0);
    yyjson_val *root = document ? yyjson_doc_get_root(document) : NULL;
    yyjson_val *draft = yyjson_is_obj(root)
        ? yyjson_obj_get(root, "draft") : NULL;
    yyjson_val *prerelease = yyjson_is_obj(root)
        ? yyjson_obj_get(root, "prerelease") : NULL;
    bool valid = yyjson_is_obj(root) && yyjson_is_bool(draft) &&
        yyjson_is_bool(prerelease) && !yyjson_get_bool(draft) &&
        !yyjson_get_bool(prerelease);
    char tag[BONGO_CAT_UPDATE_VERSION_CAP + 1] = {0};
    if (valid) valid = copy_text(tag, sizeof(tag),
        yyjson_obj_get(root, "tag_name"), true, false);
    const char *version = tag[0] == 'v' || tag[0] == 'V' ? tag + 1 : tag;
    if (valid) valid = bongo_cat_update_version_valid(version);
    if (valid) snprintf(release->version, sizeof(release->version), "%s",
        version);
    if (valid) valid = copy_text(release->release_url,
        sizeof(release->release_url), yyjson_obj_get(root, "html_url"), true,
        false) &&
        safe_url(release->release_url, RELEASE_PREFIX);
    /* 说明必须在资源之前解析: 它是期望哈希的第二来源, read_assets 会用到。 */
    if (valid) {
        yyjson_val *body = yyjson_obj_get(root, "body");
        if (body && !copy_text(release->notes, sizeof(release->notes),
                body, false, true)) valid = false;
    }
    if (valid) valid = read_assets(root, platform, release);
    yyjson_doc_free(document);
    if (!valid) bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
        "GitHub returned invalid BongoCat release metadata");
    return valid;
}
