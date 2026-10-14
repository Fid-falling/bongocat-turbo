#include "test.h"
#include "bongo_cat/file.h"
#include "bongo_cat/i18n.h"
#include "bongo_cat/path.h"
#include "bongo_cat/sha256.h"
#include "bongo_cat/update.h"
#include "bongo_cat/update_source.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
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
    /*
     * 一键更新的每一句阶段原文都必须在**所有 10 种语言**里有对应的人话:
     * 这些文案会直接出现在"下载失败/校验不通过/请手动替换"这些用户最需要看懂的场合,
     * 退回英文原文 (或空串) 等于没提示。CheckLocalization 只保证 key 存在, 这里保证
     * 取值不是兜底值 —— 两者一起才说明翻译真的接上了。
     */
    const char *markers[] = {
        BONGO_CAT_UPDATE_ERROR_NO_CHECKSUM,
        BONGO_CAT_UPDATE_ERROR_HASH_MISMATCH,
        BONGO_CAT_UPDATE_ERROR_UNREADABLE,
        BONGO_CAT_UPDATE_ERROR_SOURCES_EXHAUSTED,
        BONGO_CAT_UPDATE_ERROR_NOT_WRITABLE,
        BONGO_CAT_UPDATE_ERROR_REPLACE_FAILED
    };
    for (int language = 0; language < BONGO_CAT_LANG_COUNT; ++language) {
        BongoCatI18n *i18n = open_locale((BongoCatLanguage)language);
        CHECK(i18n != NULL);
        if (!i18n) continue;
        for (size_t index = 0; index < sizeof(markers) / sizeof(markers[0]);
                ++index) {
            const char *text = bongo_cat_update_error_text(i18n,
                markers[index]);
            CHECK(text != NULL && text[0]);
            /* 映射必须命中: 命中不了就会把英文原文(network error 那种)直接甩给用户 */
            CHECK(strcmp(text, markers[index]) != 0);
        }
        /* 每个语言都必须真的翻好这几句: 它们是"没装成 + 当前版本还在 + 下一步做什么"
           的唯一载体, 缺一句用户就会以为程序坏了。 */
        const char *required[] = {
            "native.support.updateUnverifiable",
            "native.support.updateHashMismatch",
            "native.support.updateDownloadFailed",
            "native.support.updateManualReplace",
            "native.support.updateDownloading",
            "native.support.updateStatusDownloading",
            "native.support.updateStatusVerifying",
            "native.support.updateStatusInstalling",
            "native.support.updateStatusManual",
            "native.support.updateStatusSource",
            "native.support.updateVerifying",
            "native.support.updateInstalling",
            "native.support.updateDownloadingUnknown"
        };
        const char *missing_key = "__missing_translation__";
        for (size_t index = 0; index < sizeof(required) / sizeof(required[0]);
                ++index) {
            const char *text = bongo_cat_i18n_get(i18n, required[index],
                missing_key);
            CHECK(strcmp(text, missing_key) != 0);
            CHECK(text[0] != '\0');
        }
        bongo_cat_i18n_destroy(i18n);
    }
}

/* 一键更新相关的新用例定义在本文件后半部分 (与早先的检查更新用例分开写), 这里先声明,
   免得把 test_update 排到文件末尾。 */
static void test_update_checksum_text(void);
static void test_update_release_checksum(void);
static void test_update_verify_download(void);
static void test_update_sources(void);

void test_update(void) {
    test_update_error_text();
    test_update_checksum_text();
    test_update_release_checksum();
    test_update_verify_download();
    test_update_sources();
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
    CHECK(strstr(release.installer.url, "windows-x64-setup.exe") != NULL);
    CHECK(strstr(release.portable.url, "windows-x64-portable.exe") != NULL);
    CHECK(strcmp(release.notes, "Fixed input and rendering.") == 0);
    /* 这份元数据里既没有 digest 也没有说明哈希, 但有 CI 一定会发布的 .sha256 资产:
       期望哈希的来源必须是第三级 (校验资产), 并且只抄官方地址。 */
    CHECK(release.portable.checksum == BONGO_CAT_UPDATE_CHECKSUM_ASSET);
    CHECK(strstr(release.portable.checksum_url,
        "windows-x64-portable.exe.sha256") != NULL);
    CHECK(release.installer.checksum == BONGO_CAT_UPDATE_CHECKSUM_NONE);

    CHECK(bongo_cat_update_parse_release(unix_release_json, "linux-x64",
        &release, &error));
    CHECK(release.installer.url[0] == '\0');
    CHECK(strstr(release.portable.url, "linux-x64.tar.gz") != NULL);
    CHECK(release.portable.checksum == BONGO_CAT_UPDATE_CHECKSUM_NONE);

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

/*
 * 期望哈希的文本解析。这是整条自动更新链路里唯一"信任"的输入, 所以既要接受真实世界
 * 会出现的几种写法 (digest 的 sha256: 前缀、说明里的 SHA256 (x) = y、sha256sum 的两列
 * 格式), 也必须对任何含糊形态说不 —— 猜一个哈希等于没有校验。
 */
static void test_update_checksum_text(void) {
    const char *hash = "0123456789abcdef0123456789abcdef0123456789abcdef"
        "0123456789abcdef";
    char digest[BONGO_CAT_UPDATE_DIGEST_CAP];
    CHECK(bongo_cat_update_sha256_text(hash, digest));
    CHECK(strcmp(digest, hash) == 0);
    /* digest 字段的真实形态带前缀, 大写也要能归一 */
    CHECK(bongo_cat_update_sha256_text("sha256:0123456789ABCDEF0123456789ABCDEF"
        "0123456789ABCDEF0123456789ABCDEF", digest));
    CHECK(strcmp(digest, hash) == 0);
    CHECK(bongo_cat_update_sha256_text("  0123456789abcdef0123456789abcdef"
        "0123456789abcdef0123456789abcdef  ", digest));
    CHECK(!bongo_cat_update_sha256_text("0123456789abcdef", digest));
    CHECK(!bongo_cat_update_sha256_text(
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeZ",
        digest));
    CHECK(!bongo_cat_update_sha256_text(
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0",
        digest));
    CHECK(!bongo_cat_update_sha256_text("", digest));
    CHECK(!bongo_cat_update_sha256_text(NULL, digest));

    /* 说明正文: 真实 release 说明是 markdown, 行首可能有列表符号, 也可能写成小写 */
    const char notes[] =
        "## Download\r\n"
        "- [Windows x64 portable](https://example.invalid/a.exe)\r\n"
        "\r\n"
        "sha256 (BongoCat-1.2.3-windows-x64-portable.exe) = "
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\r\n";
    CHECK(bongo_cat_update_notes_sha256(notes,
        "BongoCat-1.2.3-windows-x64-portable.exe", digest));
    CHECK(strcmp(digest, hash) == 0);
    /* 资产名对不上就不能用: 那多半是另一个架构/另一个文件的哈希 */
    CHECK(!bongo_cat_update_notes_sha256(notes,
        "BongoCat-1.2.3-windows-x86-portable.exe", digest));
    CHECK(!bongo_cat_update_notes_sha256("no checksums here",
        "BongoCat-1.2.3-windows-x64-portable.exe", digest));

    /* CI 生成的 <资产名>.sha256 是 sha256sum 的两列格式; 只有一行哈希也认 */
    char line[256];
    snprintf(line, sizeof(line), "%s  BongoCat-1.2.3-windows-x64-portable.exe\n",
        hash);
    CHECK(bongo_cat_update_checksum_file_sha256(line,
        "BongoCat-1.2.3-windows-x64-portable.exe", digest));
    CHECK(strcmp(digest, hash) == 0);
    snprintf(line, sizeof(line), "%s  BongoCat-1.2.3-windows-x86-portable.exe\n",
        hash);
    CHECK(!bongo_cat_update_checksum_file_sha256(line,
        "BongoCat-1.2.3-windows-x64-portable.exe", digest));
    snprintf(line, sizeof(line), "%s\n", hash);
    CHECK(bongo_cat_update_checksum_file_sha256(line,
        "BongoCat-1.2.3-windows-x64-portable.exe", digest));
    CHECK(!bongo_cat_update_checksum_file_sha256("not a checksum file",
        "BongoCat-1.2.3-windows-x64-portable.exe", digest));
}

/* 三级回退的优先级: digest > 说明 > 校验资产; 都没有就必须是 NONE (运行时据此拒绝更新)。 */
static void test_update_release_checksum(void) {
    const char *hash = "0123456789abcdef0123456789abcdef0123456789abcdef"
        "0123456789abcdef";
    char digest_json[2048];
    snprintf(digest_json, sizeof(digest_json),
        "{\"tag_name\":\"v3.0.0\",\"draft\":false,\"prerelease\":false,"
        "\"html_url\":\"https://github.com/Fid-falling/bongocat-turbo/releases/tag/v3.0.0\","
        "\"body\":\"SHA256 (BongoCat-3.0.0-windows-x64-portable.exe) = "
        "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff\","
        "\"assets\":[{\"name\":\"BongoCat-3.0.0-windows-x64-portable.exe\","
        "\"digest\":\"sha256:%s\",\"browser_download_url\":"
        "\"https://github.com/Fid-falling/bongocat-turbo/releases/download/v3.0.0/"
        "BongoCat-3.0.0-windows-x64-portable.exe\"}]}", hash);
    BongoCatUpdateRelease release;
    BongoCatError error = {0};
    CHECK(bongo_cat_update_parse_release(digest_json, "windows-x64", &release,
        &error));
    CHECK(release.portable.checksum == BONGO_CAT_UPDATE_CHECKSUM_DIGEST);
    CHECK(strcmp(release.portable.sha256, hash) == 0);

    /* 没有 digest: 退到说明里的那一行 (需求里点名的第二种来源) */
    char notes_json[2048];
    snprintf(notes_json, sizeof(notes_json),
        "{\"tag_name\":\"v3.0.0\",\"draft\":false,\"prerelease\":false,"
        "\"html_url\":\"https://github.com/Fid-falling/bongocat-turbo/releases/tag/v3.0.0\","
        "\"body\":\"SHA256 (BongoCat-3.0.0-windows-x64-portable.exe) = %s\","
        "\"assets\":[{\"name\":\"BongoCat-3.0.0-windows-x64-portable.exe\","
        "\"browser_download_url\":"
        "\"https://github.com/Fid-falling/bongocat-turbo/releases/download/v3.0.0/"
        "BongoCat-3.0.0-windows-x64-portable.exe\"}]}", hash);
    CHECK(bongo_cat_update_parse_release(notes_json, "windows-x64", &release,
        &error));
    CHECK(release.portable.checksum == BONGO_CAT_UPDATE_CHECKSUM_NOTES);
    CHECK(strcmp(release.portable.sha256, hash) == 0);

    /* 两者都没有: checksum 必须是 NONE, 而且不能凭空造一个哈希出来 */
    const char no_checksum_json[] =
        "{\"tag_name\":\"v3.0.0\",\"draft\":false,\"prerelease\":false,"
        "\"html_url\":\"https://github.com/Fid-falling/bongocat-turbo/releases/tag/v3.0.0\","
        "\"body\":\"No checksums in this release.\",\"assets\":["
        "{\"name\":\"BongoCat-3.0.0-windows-x64-portable.exe\","
        "\"browser_download_url\":"
        "\"https://github.com/Fid-falling/bongocat-turbo/releases/download/v3.0.0/"
        "BongoCat-3.0.0-windows-x64-portable.exe\"}]}";
    CHECK(bongo_cat_update_parse_release(no_checksum_json, "windows-x64",
        &release, &error));
    CHECK(release.portable.checksum == BONGO_CAT_UPDATE_CHECKSUM_NONE);
    CHECK(release.portable.sha256[0] == '\0');

    /* 说明里写了哈希、但镜像/中间人把下载地址换成了别的域名: 整份元数据判为非法 */
    const char foreign_json[] =
        "{\"tag_name\":\"v3.0.0\",\"draft\":false,\"prerelease\":false,"
        "\"html_url\":\"https://github.com/Fid-falling/bongocat-turbo/releases/tag/v3.0.0\","
        "\"assets\":[{\"name\":\"BongoCat-3.0.0-windows-x64-portable.exe\","
        "\"digest\":\"sha256:0123456789abcdef0123456789abcdef0123456789abcdef"
        "0123456789abcdef\","
        "\"browser_download_url\":\"https://evil.example/BongoCat.exe\"}]}";
    CHECK(bongo_cat_update_parse_release(foreign_json, "windows-x64", &release,
        &error));
    CHECK(release.portable.url[0] == '\0');
    CHECK(release.portable.checksum == BONGO_CAT_UPDATE_CHECKSUM_NONE);
}

/* 临时文件路径: 用例要在真实文件系统上验证"校验通过留文件 / 不通过删文件"。 */
static bool update_test_temp_path(char *output, size_t capacity,
    const char *name) {
#ifdef _WIN32
    const char *root = getenv("TEMP");
    if (!root || !root[0]) root = ".";
#else
    const char *root = getenv("TMPDIR");
    if (!root || !root[0]) root = "/tmp";
#endif
    return bongo_cat_path_join(output, capacity, root, name);
}

/*
 * 下载后重算比对: 这条是安全关键路径。
 * 一致 -> 文件留着 (接下来才允许替换); 不一致 -> 必须删除文件并给出可本地化的原文;
 * 读不出来 (文件不存在) -> 同样不能留下任何东西。
 */
static void test_update_verify_download(void) {
    const char content[] = "bongocat update payload for the SHA-256 test";
    char path[BONGO_CAT_PATH_CAP];
    CHECK(update_test_temp_path(path, sizeof(path),
        "bongocat-update-verify-test.bin"));
    bongo_cat_path_remove(path);
    FILE *file = bongo_cat_file_open(path, "wb");
    CHECK(file != NULL);
    if (!file) return;
    CHECK(fwrite(content, 1, sizeof(content) - 1, file) == sizeof(content) - 1);
    fclose(file);
    char expected[BONGO_CAT_UPDATE_DIGEST_CAP];
    bongo_cat_sha256_bytes(content, sizeof(content) - 1, expected);
    BongoCatError error = {0};
    CHECK(bongo_cat_update_verify_download(path, expected, &error) ==
        BONGO_CAT_OK);
    CHECK(bongo_cat_path_is_file(path));

    memset(expected, '0', BONGO_CAT_UPDATE_DIGEST_CAP - 1);
    expected[BONGO_CAT_UPDATE_DIGEST_CAP - 1] = '\0';
    BongoCatError mismatch = {0};
    CHECK(bongo_cat_update_verify_download(path, expected, &mismatch) !=
        BONGO_CAT_OK);
    CHECK(strcmp(mismatch.message, BONGO_CAT_UPDATE_ERROR_HASH_MISMATCH) == 0);
    CHECK(!bongo_cat_path_is_file(path));

    BongoCatError missing = {0};
    CHECK(bongo_cat_update_verify_download(path, expected, &missing) !=
        BONGO_CAT_OK);
    CHECK(strcmp(missing.message, BONGO_CAT_UPDATE_ERROR_UNREADABLE) == 0);
    CHECK(!bongo_cat_path_is_file(path));
}

/*
 * 下载源列表: 默认列表、用户覆盖、URL 拼装 (前缀 / {url} 占位 / 官方直连)、
 * "上次成功的源提到最前"。镜像不可信这一点不由这里保证 (哈希才是信任根), 但"按顺序
 * 尝试"和"记住可用源"这两条行为全在这里。
 */
static void test_update_sources(void) {
    BongoCatUpdateSourceList list;
    bongo_cat_update_source_list_default(&list);
    CHECK(list.count >= 2);
    CHECK(strcmp(list.entries[0], BONGO_CAT_UPDATE_SOURCE_DIRECT) == 0);

    const char *text =
        "# 自定义镜像\r\n"
        "\r\n"
        "https://mirror.example/{url}\r\n"
        "https://mirror.example/{url}\r\n"
        "https://ghproxy.net/\r\n"
        "not a url\r\n"
        "ftp://bad.example/\r\n"
        "direct\r\n";
    CHECK(bongo_cat_update_source_list_parse(text, &list) == 3);
    CHECK(strcmp(list.entries[0], "https://mirror.example/{url}") == 0);
    CHECK(strcmp(list.entries[2], "direct") == 0);
    /* 解析不出任何条目时调用方必须保留默认列表 (返回 0 是信号, 不是空列表) */
    CHECK(bongo_cat_update_source_list_parse("# nothing here\n", &list) == 0);

    const char *origin =
        "https://github.com/Fid-falling/bongocat-turbo/releases/download/v9/"
        "BongoCat-9.0.0-windows-x64-portable.exe";
    char url[BONGO_CAT_UPDATE_URL_CAP * 2];
    CHECK(bongo_cat_update_source_url("direct", origin, url, sizeof(url)));
    CHECK(strcmp(url, origin) == 0);
    CHECK(bongo_cat_update_source_url("https://ghproxy.net/", origin, url,
        sizeof(url)));
    CHECK(strncmp(url, "https://ghproxy.net/https://github.com/", 38) == 0);
    /* 前缀漏写结尾斜杠要自动补, 否则拼出来是 https://ghproxy.nethttps://... */
    CHECK(bongo_cat_update_source_url("https://ghproxy.net", origin, url,
        sizeof(url)));
    CHECK(strncmp(url, "https://ghproxy.net/https://github.com/", 38) == 0);
    CHECK(bongo_cat_update_source_url("https://mirror.example/?u={url}", origin,
        url, sizeof(url)));
    CHECK(strncmp(url, "https://mirror.example/?u=https://github.com/", 44) == 0);
    CHECK(!bongo_cat_update_source_url("bad entry", origin, url, sizeof(url)));

    char label[BONGO_CAT_UPDATE_SOURCE_LABEL_CAP];
    bongo_cat_update_source_label("https://ghproxy.net/", label, sizeof(label));
    CHECK(strcmp(label, "ghproxy.net") == 0);
    bongo_cat_update_source_label(BONGO_CAT_UPDATE_SOURCE_DIRECT, label,
        sizeof(label));
    CHECK(strcmp(label, "GitHub") == 0);

    bongo_cat_update_source_list_default(&list);
    /* 先抄一份: first 是指向数组内部的借用指针, promote 会把它指的那一格覆盖掉 */
    char first[BONGO_CAT_UPDATE_SOURCE_ENTRY_CAP];
    snprintf(first, sizeof(first), "%s", list.entries[0]);
    bongo_cat_update_source_list_promote(&list, 2);
    /* 提到最前 = 后面的整体后移一位, 原来的首位落到 [1] */
    CHECK(strcmp(list.entries[0], "https://gh-proxy.com/") == 0);
    CHECK(strcmp(list.entries[1], first) == 0);
    CHECK(list.count == 4);
    bongo_cat_update_source_list_promote(&list, 99);
    CHECK(strcmp(list.entries[0], "https://gh-proxy.com/") == 0);
}
