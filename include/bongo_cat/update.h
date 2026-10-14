#ifndef BONGO_CAT_UPDATE_H
#define BONGO_CAT_UPDATE_H

#include "bongo_cat/common.h"
#include "bongo_cat/i18n.h"

#define BONGO_CAT_UPDATE_URL_CAP 512
#define BONGO_CAT_UPDATE_NOTES_CAP 8192
#define BONGO_CAT_UPDATE_ASSET_CAP 128
/* SHA-256 的文本形态固定成 64 个小写十六进制字符 + 结尾 NUL。整个"下载→校验→替换"
   链路里只搬运这一种形态, 比较就是逐字节 strcmp, 不做大小写/前缀的二次归一。 */
#define BONGO_CAT_UPDATE_DIGEST_CAP 65

/*
 * 阶段失败时写进 service->error 的英文原文。定义在这里只有一个原因: 写入方
 * (update_service.c / update_download.c / core/update_digest.c) 与翻译方
 * (core/update_error.c) 必须逐字节一致, 用宏代替"两处手抄同一个字符串"。
 * 界面上看到的一律是 bongo_cat_update_error_text 给出的本地化人话, 这些原文只进日志。
 */
#define BONGO_CAT_UPDATE_ERROR_NO_CHECKSUM \
    "The release has no checksum for this file"
#define BONGO_CAT_UPDATE_ERROR_HASH_MISMATCH \
    "Downloaded update file failed the SHA-256 check"
#define BONGO_CAT_UPDATE_ERROR_UNREADABLE \
    "Cannot read the downloaded update file"
#define BONGO_CAT_UPDATE_ERROR_SOURCES_EXHAUSTED \
    "Update download failed: all sources exhausted"
#define BONGO_CAT_UPDATE_ERROR_NOT_WRITABLE \
    "The installation directory is not writable"
#define BONGO_CAT_UPDATE_ERROR_REPLACE_FAILED \
    "Cannot replace the running application"

/*
 * 期望哈希从哪来。这是整条自动更新链路的信任根: 镜像只搬运 exe 本体, 从来不带哈希,
 * 所以镜像被投毒也换不掉用户手上的程序 —— 见 runtime/update/update_download.c。
 */
typedef enum BongoCatUpdateChecksum {
    /* 一个都没拿到: 不下载、不替换, 只提示"无法校验, 已取消"。 */
    BONGO_CAT_UPDATE_CHECKSUM_NONE = 0,
    /* GitHub API 的 asset.digest ("sha256:<hex>"), 首选。 */
    BONGO_CAT_UPDATE_CHECKSUM_DIGEST,
    /* release 说明里的 "SHA256 (<资产名>) = <hex>" 行。 */
    BONGO_CAT_UPDATE_CHECKSUM_NOTES,
    /* 同一 release 里的 <资产名>.sha256 资产: 需要额外抓一次, 仍只从官方源抓。 */
    BONGO_CAT_UPDATE_CHECKSUM_ASSET
} BongoCatUpdateChecksum;

/*
 * 一个发布资产 = 文件名 + 下载地址 + 期望 SHA-256 及其来源。
 * name/url 由 core/update_release.c 校验过 (固定仓库前缀 + 文件名完全一致), sha256
 * 为空时 checksum 必然是 NONE, 调用方据此禁止替换。
 */
typedef struct BongoCatUpdateAsset {
    char name[BONGO_CAT_UPDATE_ASSET_CAP];
    char url[BONGO_CAT_UPDATE_URL_CAP];
    char sha256[BONGO_CAT_UPDATE_DIGEST_CAP];
    BongoCatUpdateChecksum checksum;
    /* checksum == CHECKSUM_ASSET 时有效: <资产名>.sha256 的官方直链。 */
    char checksum_url[BONGO_CAT_UPDATE_URL_CAP];
} BongoCatUpdateAsset;

typedef struct BongoCatUpdateRelease {
    char version[BONGO_CAT_UPDATE_VERSION_CAP];
    char release_url[BONGO_CAT_UPDATE_URL_CAP];
    /* 安装器 (setup.exe): 只用于"打开发布页让用户手动装", 不参与自动替换 —— 自动替换
       装的必须是单文件 exe, 也就是 portable。 */
    BongoCatUpdateAsset installer;
    /* 便携版单文件 exe: 与 BongoCatTurbo.exe 逐字节同源 (见 cmake/Packaging.cmake 的
       package-portable), 自动更新的载荷就是它。 */
    BongoCatUpdateAsset portable;
    char notes[BONGO_CAT_UPDATE_NOTES_CAP];
} BongoCatUpdateRelease;

#ifdef __cplusplus
extern "C" {
#endif

int bongo_cat_update_compare_versions(const char *left, const char *right);
bool bongo_cat_update_version_valid(const char *version);
bool bongo_cat_update_parse_release(const char *json, const char *platform,
    BongoCatUpdateRelease *release, BongoCatError *error);
const char *bongo_cat_update_error_text(const BongoCatI18n *i18n,
    const char *raw);

/*
 * 哈希工具 (core/update_digest.c)。三条不变量:
 * 1. 只有 64 个十六进制字符才算哈希, 带 "sha256:" 前缀也接受, 但一律归一成小写;
 * 2. 解析失败返回 false 而不是"猜一个", 调用方必须据此放弃更新;
 * 3. verify 负责删除可疑文件 —— 校验不通过时绝不把文件留在磁盘上给别的路径复用。
 */
bool bongo_cat_update_sha256_text(const char *text,
    char output[BONGO_CAT_UPDATE_DIGEST_CAP]);
bool bongo_cat_update_notes_sha256(const char *notes, const char *asset,
    char output[BONGO_CAT_UPDATE_DIGEST_CAP]);
bool bongo_cat_update_checksum_file_sha256(const char *content,
    const char *asset, char output[BONGO_CAT_UPDATE_DIGEST_CAP]);
/*
 * 重算 path 的 SHA-256 并与 expected 比较 (expected 必须是归一后的 64 位小写十六进制)。
 * 一致返回 BONGO_CAT_OK; 不一致或读不了返回失败, 并**删除**该文件 (下载失败/被篡改的
 * 临时文件留着只会误导用户, 见函数内注释)。error 里放的是英文原文, 由 update_error.c
 * 翻成人话。
 */
BongoCatResult bongo_cat_update_verify_download(const char *path,
    const char *expected, BongoCatError *error);

#ifdef __cplusplus
}
#endif

#endif
