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
 */
#define RELEASE_PREFIX \
    "https://github.com/Fid-falling/bongocat-vulkan/releases/tag/"
#define DOWNLOAD_PREFIX \
    "https://github.com/Fid-falling/bongocat-vulkan/releases/download/"

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

/* 必须落在预期前缀(写死的 https://github.com/Fid-falling/bongocat-vulkan/...)之下,
   且不能含控制字符或反斜杠: 前者防伪造域名, 后者防止 URL 在 Windows 上被当成
   本地路径解析。 */
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

/* 按平台拼出期望的安装包 / 便携包文件名。找不到对应资源不算错误, 只是对应字段
   保持为空, 由 UI 决定提示哪一种更新方式。 */
static bool read_assets(yyjson_val *root, const char *platform,
    BongoCatUpdateRelease *release) {
    yyjson_val *assets = yyjson_obj_get(root, "assets");
    if (!yyjson_is_arr(assets)) return false;
    char installer[128] = {0}, portable[128] = {0};
    int installer_length = 0, portable_length = 0;
    if (strncmp(platform, "windows-", 8) == 0) {
        installer_length = snprintf(installer, sizeof(installer),
            "BongoCat-%s-%s-setup.exe", release->version, platform);
        portable_length = snprintf(portable, sizeof(portable),
            "BongoCat-%s-%s-portable.exe", release->version, platform);
    } else if (strcmp(platform, "linux-x64") == 0) {
        portable_length = snprintf(portable, sizeof(portable),
            "BongoCat-%s-%s.tar.gz", release->version, platform);
    } else if (strcmp(platform, "macos-x64") == 0 ||
        strcmp(platform, "macos-arm64") == 0) {
        portable_length = snprintf(portable, sizeof(portable),
            "BongoCat-%s-%s.zip", release->version, platform);
    } else return false;
    if (installer_length < 0 || (size_t)installer_length >= sizeof(installer) ||
        portable_length < 0 || (size_t)portable_length >= sizeof(portable))
        return false;
    size_t index, count;
    yyjson_val *asset;
    yyjson_arr_foreach(assets, index, count, asset) {
        if (!yyjson_is_obj(asset)) continue;
        if (installer[0] && !release->installer_url[0])
            copy_asset_url(asset, installer, release->installer_url,
                sizeof(release->installer_url));
        if (!release->portable_url[0]) copy_asset_url(asset, portable,
            release->portable_url, sizeof(release->portable_url));
    }
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
