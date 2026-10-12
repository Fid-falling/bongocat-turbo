#include "test.h"
#include "bongo_cat/i18n.h"
#include "bongo_cat/path.h"
#include "bongo_cat/update.h"

#include <stddef.h>
#include <string.h>

static const char release_json[] =
    "{"
    "\"tag_name\":\"v1.2.3\","
    "\"html_url\":\"https://github.com/Fid-falling/bongocat-turbo/releases/tag/v1.2.3\","
    "\"draft\":false,\"prerelease\":false,"
    "\"body\":\"Fixed input and rendering.\","
    "\"assets\":["
    "{\"name\":\"BongoCat-1.2.3-windows-x64-portable.exe.sha256\","
    "\"browser_download_url\":\"https://github.com/Fid-falling/bongocat-turbo/"
    "releases/download/v1.2.3/BongoCat-1.2.3-windows-x64-portable.exe.sha256\"},"
    "{\"name\":\"BongoCat-1.2.3-windows-x64-setup.exe\","
    "\"browser_download_url\":\"https://github.com/Fid-falling/bongocat-turbo/"
    "releases/download/v1.2.3/BongoCat-1.2.3-windows-x64-setup.exe\"},"
    "{\"name\":\"BongoCat-1.2.3-windows-x64-portable.exe\","
    "\"browser_download_url\":\"https://github.com/Fid-falling/bongocat-turbo/"
    "releases/download/v1.2.3/BongoCat-1.2.3-windows-x64-portable.exe\"}]}";

static const char unix_release_json[] =
    "{\"tag_name\":\"v2.0.0\","
    "\"html_url\":\"https://github.com/Fid-falling/bongocat-turbo/releases/tag/v2.0.0\","
    "\"draft\":false,\"prerelease\":false,\"assets\":["
    "{\"name\":\"BongoCat-2.0.0-linux-x64.tar.gz\","
    "\"browser_download_url\":\"https://github.com/Fid-falling/bongocat-turbo/"
    "releases/download/v2.0.0/BongoCat-2.0.0-linux-x64.tar.gz\"}]}";

/* 起一个真实的 i18n: 语言文件就在源码树里, 与 i18n 用例用同一个编译期宏定位。
   失败返回 NULL, 由调用方记一次失败并跳过后续断言。 */
static BongoCatI18n *open_locale(BongoCatLanguage language) {
    char root[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(root, sizeof(root), BONGO_CAT_NATIVE_SOURCE_DIR,
            "resources/assets/locales")) return NULL;
    BongoCatError error = {0};
    return bongo_cat_i18n_create(root, language, &error);
}

/* 那句状态码前缀在两处网络实现里各写了一遍(Windows 用 %lu, Unix 用 %ld),
   core/update_error.c 靠它认状态码 —— 任何一处改了措辞, 界面就会退回英文原文。
   限流复现不了, 所以这里直接读源码, 把这个跨文件的约定钉住。 */
static bool source_writes_status_prefix(const char *relative) {
    char path[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(path, sizeof(path), BONGO_CAT_NATIVE_SOURCE_DIR,
            relative)) return false;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    char buffer[32768];
    size_t length = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    buffer[length] = '\0';
    return strstr(buffer, "\"GitHub returned HTTP status %") != NULL;
}

/*
 * 检查更新失败时界面拿到的话。403/429 是 GitHub 匿名接口限流(每 IP 每小时 60 次),
 * 用户看到的必须是"接口暂时限流, 稍后重试, 也可以直接看发布页"这种人话 ——
 * 之前直接把 "GitHub returned HTTP status 403" 显示出来, 既看不懂也没有出路。
 * 限流无法随时复现, 所以这里直接喂网络层(update_http.c / update_http_unix.c)
 * 写出的原始文案, 断言的就是那条链路末端的结果。
 */
static void test_update_error_text(void) {
    const BongoCatLanguage languages[] = {
        BONGO_CAT_LANG_ZH_CN, BONGO_CAT_LANG_EN_US
    };
    /* 状态码与"HTTP 状态码 / HTTP status"这类术语都不该出现在界面文案里,
       它们只留在日志(update_service.c 记录的是原始 error 文本)。 */
    const char *forbidden[] = {
        "HTTP", "状态码", "status code", "403", "429", "500"
    };
    const char *statuses[] = {
        "GitHub returned HTTP status 403", "GitHub returned HTTP status 429"
    };
    for (size_t index = 0; index < sizeof(languages) / sizeof(languages[0]);
            ++index) {
        BongoCatI18n *i18n = open_locale(languages[index]);
        CHECK(i18n != NULL);
        if (!i18n) continue;
        const char *missing = "__missing_translation__";
        const char *limited = bongo_cat_i18n_get(i18n,
            "native.support.updateRateLimited", missing);
        CHECK(strcmp(limited, missing) != 0);
        for (size_t status = 0; status < sizeof(statuses) /
                sizeof(statuses[0]); ++status) {
            const char *text = bongo_cat_update_error_text(i18n,
                statuses[status]);
            CHECK(strcmp(text, limited) == 0);
            for (size_t word = 0; word < sizeof(forbidden) /
                    sizeof(forbidden[0]); ++word)
                CHECK(strstr(text, forbidden[word]) == NULL);
        }
        /* 其它状态码走通用文案, 同样不把状态码甩给用户 */
        const char *generic = bongo_cat_update_error_text(i18n,
            "GitHub returned HTTP status 500");
        CHECK(strcmp(generic, bongo_cat_i18n_get(i18n,
            "native.support.updateHttpFailed", missing)) == 0);
        for (size_t word = 0; word < sizeof(forbidden) / sizeof(forbidden[0]);
                ++word)
            CHECK(strstr(generic, forbidden[word]) == NULL);
        /* 认不出来的原文原样透传(网络错误仍是英文), 不吞信息; 空串退到"无法检查更新" */
        CHECK(strcmp(bongo_cat_update_error_text(i18n,
            "Network error: network timeout (code 12002)"),
            "Network error: network timeout (code 12002)") == 0);
        CHECK(strcmp(bongo_cat_update_error_text(i18n, ""),
            bongo_cat_i18n_get(i18n, "native.support.updateFailed",
                missing)) == 0);
        bongo_cat_i18n_destroy(i18n);
    }
    /* i18n 还没建起来(极早期失败)时也要给一句英文兜底, 不能返回空指针 */
    const char *fallback = bongo_cat_update_error_text(NULL,
        "GitHub returned HTTP status 403");
    CHECK(fallback != NULL && fallback[0] != '\0');
    CHECK(strstr(fallback, "HTTP") == NULL);
    CHECK(source_writes_status_prefix("src/runtime/update/update_http.c"));
    CHECK(source_writes_status_prefix(
        "src/runtime/update/update_http_unix.c"));
}

void test_update(void) {
    test_update_error_text();
    CHECK(bongo_cat_update_version_valid("1.2.3-rc.1+build.4"));
    CHECK(!bongo_cat_update_version_valid("1"));
    CHECK(!bongo_cat_update_version_valid("1."));
    CHECK(!bongo_cat_update_version_valid("1.2"));
    CHECK(bongo_cat_update_compare_versions("1.2.3", "1.2.2") > 0);
    CHECK(bongo_cat_update_compare_versions("v2.0.0", "1.99.99") > 0);
    CHECK(bongo_cat_update_compare_versions("1.0.0", "1.0.0-rc.1") > 0);
    CHECK(bongo_cat_update_compare_versions("1.0.0-beta.2",
        "1.0.0-beta.11") < 0);
    CHECK(bongo_cat_update_compare_versions("1.0.0+build.2",
        "1.0.0+build.1") == 0);

    BongoCatUpdateRelease release;
    BongoCatError error = {0};
    CHECK(bongo_cat_update_parse_release(release_json, "windows-x64",
        &release, &error));
    CHECK(strcmp(release.version, "1.2.3") == 0);
    CHECK(strstr(release.installer_url, "windows-x64-setup.exe") != NULL);
    CHECK(strstr(release.portable_url, "windows-x64-portable.exe") != NULL);
    CHECK(strcmp(release.notes, "Fixed input and rendering.") == 0);

    CHECK(bongo_cat_update_parse_release(unix_release_json, "linux-x64",
        &release, &error));
    CHECK(release.installer_url[0] == '\0');
    CHECK(strstr(release.portable_url, "linux-x64.tar.gz") != NULL);

    const char unsafe[] =
        "{\"tag_name\":\"v2.0.0\",\"draft\":false,"
        "\"prerelease\":false,\"html_url\":\"https://example.com/v2\","
        "\"assets\":[]}";
    CHECK(!bongo_cat_update_parse_release(unsafe, "windows-x64",
        &release, &error));

    const char oversized_tag[] =
        "{\"tag_name\":\"v1234567890123456789012345678901234567890\","
        "\"draft\":false,\"prerelease\":false,"
        "\"html_url\":\"https://github.com/Fid-falling/bongocat-turbo/"
        "releases/tag/oversized\",\"assets\":[]}";
    CHECK(!bongo_cat_update_parse_release(oversized_tag, "windows-x64",
        &release, &error));
}
