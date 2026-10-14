/*
 * 下载源列表在磁盘上的两个文件, 都在配置目录 (app->config_root) 下:
 *
 *   update-sources.txt   —— 给用户/维护者改的镜像列表, 每行一条 (# 开头是注释)。
 *                           存在且能解析出至少一条时, **完全取代**内置默认列表, 这样
 *                           自建镜像的人可以把官方源也去掉。文件里可以写:
 *                             direct                        (GitHub 官方直连)
 *                             https://ghproxy.net/          (前缀 + 官方 URL)
 *                             https://mirror.example/{url}  (把官方 URL 塞进占位符)
 *   update-sources.state —— 上一次成功的源 (一行, 程序自己写)。下次优先尝试它, 免得每次
 *                           都先等官方直连超时。用户不必管这个文件, 删掉就会退回默认顺序。
 *
 * 为什么不用 settings.json: 镜像随时会失效, "改一行文本换源"比走配置 schema + 迁移 +
 * 设置界面简单得多, 也不会把会过期的东西固化进配置版本。
 * 两个文件都只有几行, 读写都在更新工作线程里做 (检查/下载时), 不新增并发面。
 */
#include "update_internal.h"
#include "bongo_cat/file.h"
#include "bongo_cat/log.h"

#include <stdio.h>
#include <string.h>

#define SOURCES_FILE "update-sources.txt"
#define SOURCES_STATE_FILE "update-sources.state"

/* 拼接配置目录下的文件名; config_root 为空 (极早期失败) 时返回 false, 调用方退回默认源。 */
static bool state_path(BongoCatUpdateService *service, const char *name,
    char *output, size_t capacity) {
    const char *root = service->app ? service->app->config_root : NULL;
    if (!root || !root[0]) return false;
    int written = snprintf(output, capacity, "%s/%s", root, name);
    return written > 0 && (size_t)written < capacity;
}

/* 读一个小文本文件到栈缓冲; 读不到 (文件不存在/没权限/太长) 都返回空串, 由调用方走默认值。 */
static void read_text(const char *path, char *output, size_t capacity) {
    output[0] = '\0';
    FILE *file = bongo_cat_file_open(path, "rb");
    if (!file) return;
    size_t length = fread(output, 1, capacity - 1, file);
    output[length] = '\0';
    fclose(file);
}

/*
 * 载入本次可用的源顺序。顺序规则:
 *   1. 用户列表 (若有) 或内置默认列表;
 *   2. 上次成功的源提到最前 —— 只在它仍出现在列表里时才会命中 (用户改过列表就自然失效)。
 * 全部失败时列表仍是完整的, 调用方按顺序逐个尝试。
 */
void bongo_cat_update_sources_load(BongoCatUpdateService *service) {
    if (!service) return;
    bongo_cat_update_source_list_default(&service->sources);
    char path[BONGO_CAT_PATH_CAP];
    char text[BONGO_CAT_UPDATE_SOURCE_TEXT_CAP];
    if (state_path(service, SOURCES_FILE, path, sizeof(path))) {
        read_text(path, text, sizeof(text));
        BongoCatUpdateSourceList configured;
        if (bongo_cat_update_source_list_parse(text, &configured) > 0)
            service->sources = configured;
    }
    if (!state_path(service, SOURCES_STATE_FILE, path, sizeof(path))) return;
    read_text(path, text, sizeof(text));
    /* 状态文件是程序自己写的一行; 只取第一行并去掉换行, 其余内容一律忽略。 */
    char *newline = strchr(text, '\n');
    if (newline) *newline = '\0';
    size_t length = strlen(text);
    while (length && (text[length - 1] == '\r' || text[length - 1] == ' '))
        text[--length] = '\0';
    if (!text[0]) return;
    for (size_t index = 0; index < service->sources.count; ++index)
        if (strcmp(service->sources.entries[index], text) == 0) {
            bongo_cat_update_source_list_promote(&service->sources, index);
            SDL_LogInfo(BONGO_CAT_LOG_UPDATE,
                "Update source remembered from the last successful download: %s",
                text);
            return;
        }
}

/* 记住这次成功的源。写失败不影响本次更新 (只是下次还得按默认顺序重试一遍), 所以只记日志。 */
void bongo_cat_update_sources_remember(BongoCatUpdateService *service,
    size_t index) {
    if (!service || index >= service->sources.count) return;
    const char *entry = service->sources.entries[index];
    char path[BONGO_CAT_PATH_CAP];
    if (state_path(service, SOURCES_STATE_FILE, path, sizeof(path))) {
        FILE *file = bongo_cat_file_open(path, "wb");
        if (file) {
            fprintf(file, "%s\n", entry);
            fclose(file);
        } else SDL_LogWarn(BONGO_CAT_LOG_UPDATE,
            "Cannot remember the update source (%s)", path);
    }
    bongo_cat_update_source_list_promote(&service->sources, index);
}
