/*
 * 期望哈希的解析与"下载后重算比对"。
 *
 * 为什么单独一层: 自动更新是唯一会把网络上的字节变成用户机器上可执行文件的路径, 而
 * 镜像/代理不可信。信任根因此只有三处**官方来源**给出的 SHA-256(见 update.h 的
 * BongoCatUpdateChecksum), 本文件负责把它们的文本形态解析成唯一的 64 位小写十六进制,
 * 并在下载完成后用同一份实现重算 —— 解析与重算必须是同一套字节口径, 否则会出现
 * "看起来校验过了"的假阳性。
 *
 * 三条不变量:
 * 1. 解析不出来就返回 false, 绝不返回"部分匹配"或零哈希;
 * 2. 归一化后的哈希只有一种形态(64 小写 hex), 比较就是 strcmp;
 * 3. verify 在校验失败时**删除**文件: 留着可疑的 .new 文件既占地方, 也可能被回滚
 *    路径或用户手工拿去替换, 那是我们要极力避免的事。
 */
#include "bongo_cat/path.h"
#include "bongo_cat/sha256.h"
#include "bongo_cat/update.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define SHA256_HEX_LENGTH 64
#define SHA256_PREFIX "sha256:"

static int lower(int value) {
    return value >= 'A' && value <= 'Z' ? value - 'A' + 'a' : value;
}

/* 十六进制字符判定用查表而不是 isxdigit: C 标准的 isxdigit 依赖 locale, 而且我们要同时
   处理大小写归一, 一张表最省事也最不容易在别的 locale 下出意外。 */
static int hex_value(int value) {
    if (value >= '0' && value <= '9') return value - '0';
    int lowered = lower(value);
    return lowered >= 'a' && lowered <= 'f' ? lowered - 'a' + 10 : -1;
}

/* 跳过空白 (空格与制表符)。换行由调用方按行处理, 这里不吞换行, 免得把两行粘成一个哈希。 */
static const char *skip_blanks(const char *text) {
    while (text && (*text == ' ' || *text == '\t')) ++text;
    return text;
}

static bool blank(int value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

/*
 * 把 64 个十六进制字符读成归一化哈希。允许 "sha256:" 前缀 (GitHub API 的 digest 形态)
 * 与前后空白, 但哈希本体之后只允许空白 —— 多一个字就说明这不是一个纯粹的哈希, 宁可
 * 判为解析失败。
 */
bool bongo_cat_update_sha256_text(const char *text,
    char output[BONGO_CAT_UPDATE_DIGEST_CAP]) {
    if (!text || !output) return false;
    const char *at = skip_blanks(text);
    if (strncmp(at, SHA256_PREFIX, sizeof(SHA256_PREFIX) - 1) == 0)
        at = skip_blanks(at + sizeof(SHA256_PREFIX) - 1);
    char normalized[SHA256_HEX_LENGTH + 1] = {0};
    for (size_t index = 0; index < SHA256_HEX_LENGTH; ++index) {
        int digit = hex_value((unsigned char)at[index]);
        if (digit < 0) return false;
        normalized[index] = (char)("0123456789abcdef"[digit]);
    }
    for (const char *tail = at + SHA256_HEX_LENGTH; *tail; ++tail)
        if (!blank((unsigned char)*tail)) return false;
    memcpy(output, normalized, sizeof(normalized));
    return true;
}

/* 大小写不敏感的前缀比较; 用它匹配 "SHA256" 这个字面词。 */
static bool prefix_ci(const char *text, const char *prefix) {
    for (size_t index = 0; prefix[index]; ++index)
        if (lower((unsigned char)text[index]) != lower((unsigned char)prefix[index]))
            return false;
    return true;
}

/* 在行内找 "<资产名>" 并返回它之后的位置; 找不到返回 NULL。资产名区分大小写:
   发布资产名是跨仓库契约 (update_release.c 拼出来的那个), 不能靠大小写模糊匹配。 */
static const char *after_asset_name(const char *line, const char *asset) {
    size_t length = strlen(asset);
    for (const char *at = line; *at; ++at)
        if (strncmp(at, asset, length) == 0) return at + length;
    return NULL;
}

/*
 * 解析 release 说明里的一行 "SHA256 (<资产名>) = <hex>"。
 * 说明正文是维护者手写/脚本生成的 markdown, 所以这里容忍: 行首空白与列表符号
 * (- * # >)、"SHA256" 的大小写、括号两侧空白、等号写成 ":" 的变体。
 * 只认行内第一个匹配; 同一资产出现两次且不一致时, 先出现的那个生效 —— 说明正文的
 * 顺序就是维护者的书写顺序, 越靠前越可能是权威值。
 */
bool bongo_cat_update_notes_sha256(const char *notes, const char *asset,
    char output[BONGO_CAT_UPDATE_DIGEST_CAP]) {
    if (!notes || !asset || !asset[0] || !output) return false;
    const char *line = notes;
    while (line && *line) {
        const char *end = strchr(line, '\n');
        size_t length = end ? (size_t)(end - line) : strlen(line);
        char buffer[512];
        if (length < sizeof(buffer)) {
            memcpy(buffer, line, length);
            buffer[length] = '\0';
            const char *at = skip_blanks(buffer);
            while (*at == '-' || *at == '*' || *at == '#' || *at == '>')
                at = skip_blanks(at + 1);
            if (prefix_ci(at, "SHA256")) {
                at = skip_blanks(at + 6);
                if (*at == '(') {
                    at = skip_blanks(at + 1);
                    const char *named = after_asset_name(at, asset);
                    if (named) {
                        named = skip_blanks(named);
                        if (*named == ')') {
                            named = skip_blanks(named + 1);
                            if (*named == '=' || *named == ':') ++named;
                            if (bongo_cat_update_sha256_text(named, output))
                                return true;
                        }
                    }
                }
            }
        }
        line = end ? end + 1 : NULL;
    }
    return false;
}

/*
 * 解析 <资产名>.sha256 文件的内容。CI 生成的是 sha256sum 的 "<hex>  <文件名>" 形态,
 * 但手工生成的校验文件常常只有一行哈希, 所以两种都认; 带了文件名就必须与期望资产名
 * 完全一致 —— 否则可能拿到的是另一个架构/另一个版本的哈希, 那比没有校验更危险
 * (校验必然失败, 用户只会看到"校验不通过")。
 */
bool bongo_cat_update_checksum_file_sha256(const char *content,
    const char *asset, char output[BONGO_CAT_UPDATE_DIGEST_CAP]) {
    if (!content || !asset || !asset[0] || !output) return false;
    char first[256] = {0};
    size_t used = 0;
    const char *at = skip_blanks(content);
    /* 说明格式也塞进校验文件的情况: 直接复用同一套解析, 免得两处口径漂移。 */
    for (size_t index = 0; at[index] && index < SHA256_HEX_LENGTH; ++index)
        if (hex_value((unsigned char)at[index]) < 0)
            return bongo_cat_update_notes_sha256(content, asset, output);
    /* 只取第一个空白之前的那个词: sha256sum 的格式是 "<hex>  <文件名>", 把整行当哈希
       会解析失败。 */
    while (*at && !blank((unsigned char)*at) && used + 1 < sizeof(first))
        first[used++] = *at++;
    first[used] = '\0';
    if (!bongo_cat_update_sha256_text(first, output)) return false;
    at = skip_blanks(at);
    if (!*at || *at == '\r' || *at == '\n') return true;
    /* 还跟着一个文件名: 去掉 sha256sum 常用的 "*" 二进制标记后必须完全相等。 */
    const char *name = *at == '*' ? at + 1 : at;
    size_t name_end = 0;
    while (name[name_end] && !blank((unsigned char)name[name_end])) ++name_end;
    return name_end == strlen(asset) && strncmp(name, asset, name_end) == 0;
}

/*
 * 重算下载文件的 SHA-256 并与期望值比较。
 * 失败一律删除文件: 半个下载、被镜像篡改的字节、读不出来的文件, 都不该留在磁盘上 ——
 * 手工替换路径会把它交给用户, 回滚路径也可能误用它。
 * expected 必须先归一 (调用方从 release 解析出来的就是归一形态), 这里不再重复归一,
 * 免得"两层都做了一点容错"反而把非法输入放进来。
 */
BongoCatResult bongo_cat_update_verify_download(const char *path,
    const char *expected, BongoCatError *error) {
    if (!path || !path[0] || !expected || !expected[0]) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_ARGUMENT,
            "Downloaded update file cannot be verified");
        return BONGO_CAT_ERROR_ARGUMENT;
    }
    char actual[BONGO_CAT_UPDATE_DIGEST_CAP] = {0};
    BongoCatError read_error = {0};
    if (bongo_cat_sha256_file(path, actual, &read_error) != BONGO_CAT_OK ||
        !actual[0]) {
        bongo_cat_path_remove(path);
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            BONGO_CAT_UPDATE_ERROR_UNREADABLE);
        return BONGO_CAT_ERROR_IO;
    }
    if (strcmp(actual, expected) != 0) {
        bongo_cat_path_remove(path);
        bongo_cat_error_set(error, BONGO_CAT_ERROR_FORMAT,
            BONGO_CAT_UPDATE_ERROR_HASH_MISMATCH);
        return BONGO_CAT_ERROR_FORMAT;
    }
    return BONGO_CAT_OK;
}
