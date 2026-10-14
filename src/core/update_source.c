/*
 * 下载源 (镜像) 列表: 解析、URL 拼装、顺序调整。
 *
 * 为什么镜像只做"前缀"而不搬运哈希: 这些公共加速服务全部由第三方运营, 谁都能被投毒
 * 或被劫持。所以它们只被允许搬运 exe 本体, 期望的 SHA-256 只会来自官方来源
 * (GitHub API 的 digest / release 说明 / 官方 .sha256 资产), 下载完成后本地重算比对。
 * 也就是说: 镜像即使返回被篡改的文件, 结果也只是"校验不通过, 更新取消"。
 *
 * 顺序策略: 列表顺序就是尝试顺序; 上次成功的源会被提到最前 (见 promote), 于是官方直连
 * 不通的用户第二次起就直接走镜像。默认列表把官方放第一位 —— 对大多数用户它是最快的,
 * 对连不上的用户最多多花一次 15 秒连接超时。
 */
#include "bongo_cat/update_source.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/*
 * 内置默认列表。**公共加速镜像随时可能失效/限流/改域名**, 失效的表现是"更新时多等一个
 * 连接超时然后自动换下一个源", 不影响正确性; 维护者在这里增删即可, 不需要改任何逻辑。
 * 用户的镜像列表文件 (config 目录下的 update-sources.txt) 一旦存在就完全取代这份默认值。
 */
static const char *const default_entries[] = {
    BONGO_CAT_UPDATE_SOURCE_DIRECT,
    "https://ghproxy.net/",
    "https://gh-proxy.com/",
    "https://ghfast.top/",
};

static bool prefix_ci(const char *text, const char *prefix) {
    for (size_t index = 0; prefix[index]; ++index)
        if (tolower((unsigned char)text[index]) !=
            tolower((unsigned char)prefix[index])) return false;
    return true;
}

static bool scheme_ok(const char *entry) {
    return prefix_ci(entry, "https://") || prefix_ci(entry, "http://");
}

/* "direct" 的大小写不敏感比较。用手写的而不是 strcasecmp(_stricmp): 后者是 POSIX/MSVC
   各一套, core 层不引 SDL, 一个 5 行的循环比平台分支干净。 */
static bool is_direct(const char *entry) {
    return entry && prefix_ci(entry, BONGO_CAT_UPDATE_SOURCE_DIRECT) &&
        entry[sizeof(BONGO_CAT_UPDATE_SOURCE_DIRECT) - 1] == '\0';
}

void bongo_cat_update_source_list_default(BongoCatUpdateSourceList *list) {
    if (!list) return;
    memset(list, 0, sizeof(*list));
    for (size_t index = 0; index < sizeof(default_entries) /
            sizeof(default_entries[0]) && index < BONGO_CAT_UPDATE_SOURCE_CAP;
            ++index)
        snprintf(list->entries[index], BONGO_CAT_UPDATE_SOURCE_ENTRY_CAP, "%s",
            default_entries[index]);
    list->count = sizeof(default_entries) / sizeof(default_entries[0]);
    if (list->count > BONGO_CAT_UPDATE_SOURCE_CAP)
        list->count = BONGO_CAT_UPDATE_SOURCE_CAP;
}

/* 条目里不允许出现空白/控制字符/反斜杠/引号: 这些字符进了 HTTP 请求行或命令行都
   可能被解释成别的东西; 反斜杠在 Windows 上还会被当成路径分隔符。 */
bool bongo_cat_update_source_valid(const char *entry) {
    if (!entry || !entry[0] || strlen(entry) >= BONGO_CAT_UPDATE_SOURCE_ENTRY_CAP)
        return false;
    if (strcmp(entry, BONGO_CAT_UPDATE_SOURCE_DIRECT) == 0) return true;
    if (!scheme_ok(entry)) return false;
    for (const unsigned char *at = (const unsigned char *)entry; *at; ++at)
        if (*at <= 0x20 || *at == 0x7f || *at == '\\' || *at == '"' ||
            *at == '\'') return false;
    return true;
}

/* 行内去尾: 去掉行尾的 CR/空格/制表符, 让 "https://x/ \r" 这样的写法也能用。 */
static void trim_tail(char *text) {
    size_t length = strlen(text);
    while (length && (text[length - 1] == '\r' || text[length - 1] == ' ' ||
        text[length - 1] == '\t')) text[--length] = '\0';
}

/* 去掉行首空白 (注释判定与条目解析共用), 顺便跳过 UTF-8 BOM: 用户用记事本另存过的
   文件经常带 BOM, 不处理的话第一行会整行失效。 */
static char *trim_head(char *text) {
    if ((unsigned char)text[0] == 0xef && (unsigned char)text[1] == 0xbb &&
        (unsigned char)text[2] == 0xbf) text += 3;
    while (*text == ' ' || *text == '\t') ++text;
    return text;
}

size_t bongo_cat_update_source_list_parse(const char *text,
    BongoCatUpdateSourceList *list) {
    if (!list) return 0;
    memset(list, 0, sizeof(*list));
    if (!text || !text[0]) return 0;
    char buffer[BONGO_CAT_UPDATE_SOURCE_TEXT_CAP];
    snprintf(buffer, sizeof(buffer), "%s", text);
    char *line = buffer;
    while (line && *line && list->count < BONGO_CAT_UPDATE_SOURCE_CAP) {
        char *end = strchr(line, '\n');
        if (end) *end = '\0';
        char *entry = trim_head(line);
        trim_tail(entry);
        if (entry[0] && entry[0] != '#' &&
            bongo_cat_update_source_valid(entry)) {
            /* 去重: 同一个源写两遍只会让失败时的等待翻倍。 */
            bool duplicate = false;
            for (size_t index = 0; index < list->count; ++index)
                if (strcmp(list->entries[index], entry) == 0) duplicate = true;
            if (!duplicate) {
                snprintf(list->entries[list->count],
                    BONGO_CAT_UPDATE_SOURCE_ENTRY_CAP, "%s", entry);
                list->count++;
            }
        }
        line = end ? end + 1 : NULL;
    }
    return list->count;
}

bool bongo_cat_update_source_url(const char *entry, const char *origin,
    char *output, size_t capacity) {
    if (!origin || !origin[0] || !output || capacity < 2) return false;
    output[0] = '\0';
    if (!entry || !entry[0] ||
        is_direct(entry)) {
        if (strlen(origin) >= capacity) return false;
        snprintf(output, capacity, "%s", origin);
        return true;
    }
    if (!bongo_cat_update_source_valid(entry)) return false;
    const char *placeholder = strstr(entry, "{url}");
    size_t used = 0;
    if (placeholder) {
        size_t head = (size_t)(placeholder - entry);
        if (head >= capacity) return false;
        memcpy(output, entry, head);
        used = head;
        int written = snprintf(output + used, capacity - used, "%s", origin);
        if (written < 0 || (size_t)written >= capacity - used) return false;
        used += (size_t)written;
        const char *tail = placeholder + 5;
        written = snprintf(output + used, capacity - used, "%s", tail);
        if (written < 0 || (size_t)written >= capacity - used) return false;
    } else {
        /* 前缀拼接: 前缀与官方 URL 之间必须有一个 '/' 分隔, 用户漏写时自动补上
           ("https://ghproxy.net" 与 "https://ghproxy.net/" 等价)。 */
        size_t length = strlen(entry);
        bool slash = entry[length - 1] == '/';
        int written = snprintf(output, capacity, slash ? "%s%s" : "%s/%s",
            entry, origin);
        if (written < 0 || (size_t)written >= capacity) return false;
    }
    /* 拼出来的东西必须还是一个 http(s) URL: 前缀里的路径再多也只能出现在主机之后,
       这里只做形态检查, 真正的信任边界仍然是哈希校验。 */
    return scheme_ok(output);
}

void bongo_cat_update_source_list_promote(BongoCatUpdateSourceList *list,
    size_t index) {
    if (!list || index == 0 || index >= list->count) return;
    char promoted[BONGO_CAT_UPDATE_SOURCE_ENTRY_CAP];
    memcpy(promoted, list->entries[index], sizeof(promoted));
    for (size_t at = index; at > 0; --at)
        memcpy(list->entries[at], list->entries[at - 1],
            sizeof(list->entries[at]));
    memcpy(list->entries[0], promoted, sizeof(promoted));
}

void bongo_cat_update_source_label(const char *entry, char *output,
    size_t capacity) {
    if (!output || !capacity) return;
    if (!entry || !entry[0] || is_direct(entry)) {
        snprintf(output, capacity, "GitHub");
        return;
    }
    const char *host = strstr(entry, "://");
    host = host ? host + 3 : entry;
    size_t length = 0;
    while (host[length] && host[length] != '/' && host[length] != '?' &&
        length + 1 < capacity) ++length;
    if (!length) {
        snprintf(output, capacity, "%s", entry);
        return;
    }
    memcpy(output, host, length);
    output[length] = '\0';
}
