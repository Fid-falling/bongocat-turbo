/*
 * 自检第 4 组: 文件与权限。
 *
 * 这一组回答"数据放得下吗、写得了吗": 存储根、exe 目录、缓存、模型、日志各一个结论,
 * 最后是磁盘剩余空间。每一项都必须给出**路径** —— 用户回传报告时, 支持要能一眼看出
 * 他把程序放在了哪 (桌面/OneDrive/只读目录是三类最常见的问题)。
 *
 * 唯一的写操作是"建一个临时文件写一个字节再删掉" (证明目录可写); 它不会碰配置、模型
 * 或任何已有文件, 也不会留下残余 (失败路径也删)。这是自检里唯一允许的落盘行为。
 */
#include "selfcheck_internal.h"

#include "bongo_cat/app.h"
#include "bongo_cat/file.h"
#include "bongo_cat/path.h"
#include "storage_paths.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include "windows_diagnostics.h"
#endif

void bongo_cat_selfcheck_parent_dir(const char *path, char *output,
    size_t capacity) {
    if (!output || capacity == 0) return;
    output[0] = '\0';
    if (!path) return;
    size_t length = strlen(path);
    while (length > 0 && (path[length - 1] == '/' || path[length - 1] == '\\'))
        --length;
    while (length > 0 && path[length - 1] != '/' && path[length - 1] != '\\')
        --length;
    while (length > 1 && (path[length - 1] == '/' || path[length - 1] == '\\'))
        --length;
    if (length == 0) return;
    snprintf(output, capacity, "%.*s", (int)length, path);
}

static bool probe_write(const char *directory, char *reason, size_t capacity) {
    if (!directory || !directory[0]) {
        snprintf(reason, capacity, "路径为空");
        return false;
    }
    if (!bongo_cat_path_is_dir(directory)) {
        snprintf(reason, capacity, "目录不存在");
        return false;
    }
    char name[64], path[BONGO_CAT_PATH_CAP];
    snprintf(name, sizeof(name), ".selfcheck-%lu.tmp", (unsigned long)SDL_GetTicks());
    if (!bongo_cat_path_join(path, sizeof(path), directory, name)) {
        snprintf(reason, capacity, "路径过长");
        return false;
    }
    FILE *file = bongo_cat_file_open(path, "wb");
    if (!file) {
        snprintf(reason, capacity, "无法创建文件 (目录不可写或路径被占用)");
        return false;
    }
    bool written = fputc('x', file) != EOF;
    if (fclose(file) != 0) written = false;
    bongo_cat_path_remove(path);
    if (!written) snprintf(reason, capacity, "写入失败 (磁盘满或无权限)");
    return written;
}

bool bongo_cat_selfcheck_dir_writable(const char *directory, char *reason,
    size_t capacity) {
    reason[0] = '\0';
    return probe_write(directory, reason, capacity);
}

/* enumerate 的访问者: 读到第一个条目就停 —— 能列出一个名字就足以证明目录可读 */
static BongoCatPathVisit readable_visit(void *userdata, const char *directory,
    const char *name) {
    (void)userdata; (void)directory; (void)name;
    return BONGO_CAT_PATH_SUCCESS;
}

bool bongo_cat_selfcheck_dir_readable(const char *directory, char *reason,
    size_t capacity) {
    reason[0] = '\0';
    if (!bongo_cat_path_is_dir(directory)) {
        snprintf(reason, capacity, "目录不存在");
        return false;
    }    /* 不能用 path_file_info 探目录: 那是给文件用的, 目录在多数实现里会被判失败。
       枚举一次才是"能读目录内容"的直接证据; 空目录在 Windows 上也被当成成功。 */
    if (!bongo_cat_path_enumerate(directory, readable_visit, NULL)) {
        snprintf(reason, capacity, "无法读取目录内容 (权限不足或被占用)");
        return false;
    }
    return true;
}

static void writable_item(BongoCatSelfCheckBuilder *builder, const char *title,
    const char *directory) {
    char reason[96];
    if (bongo_cat_selfcheck_dir_writable(directory, reason, sizeof(reason)))
        bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_PASS, title,
            "%s（可写）", directory);
    else
        bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_FAIL, title,
            "%s（%s）", directory, reason);
}

void bongo_cat_selfcheck_files(BongoCatSelfCheckBuilder *builder,
    struct BongoCatApp *app) {
    bongo_cat_selfcheck_section(builder, "文件与权限");
    char root[BONGO_CAT_PATH_CAP] = {0};
    if (bongo_cat_storage_paths_root(root, sizeof(root)) && root[0])
        writable_item(builder, "存储根", root);
    else
        bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_FAIL, "存储根",
            "未能解析存储根（--storage-root 或便携目录不可用），配置与模型都会"
            "无处可放");
    char executable_dir[BONGO_CAT_PATH_CAP];
    bongo_cat_selfcheck_parent_dir(app ? app->executable_path : NULL,
        executable_dir, sizeof(executable_dir));
    if (executable_dir[0]) writable_item(builder, "exe 目录", executable_dir);
    else
        bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_WARN, "exe 目录",
            "无法从启动路径推出 exe 目录，跳过写入检查");
    if (app) {
        char reason[96];
        if (bongo_cat_selfcheck_dir_writable(app->cache_root, reason,
                sizeof(reason)) &&
            bongo_cat_selfcheck_dir_readable(app->cache_root, reason,
                sizeof(reason)))
            bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_PASS, "缓存目录",
                "%s（可读可写）", app->cache_root);
        else
            bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_FAIL, "缓存目录",
                "%s（%s）", app->cache_root, reason);
        if (bongo_cat_selfcheck_dir_readable(app->models_root, reason,
                sizeof(reason)))
            bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_PASS, "模型目录",
                "%s（可读，共 %zu 个模型）", app->models_root, app->models.count);
        else
            bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_FAIL, "模型目录",
                "%s（%s）", app->models_root, reason);
        writable_item(builder, "日志目录", app->log_root);
    }
#ifdef _WIN32
    unsigned long long free_bytes = 0, total_bytes = 0;
    if (root[0] && bongo_cat_windows_selfcheck_disk(root, &free_bytes,
            &total_bytes)) {
        double free_gb = (double)free_bytes / (1024.0 * 1024.0 * 1024.0);
        double total_gb = (double)total_bytes / (1024.0 * 1024.0 * 1024.0);
        BongoCatSelfCheckStatus status = free_bytes < (100ull << 20) ?
            BONGO_CAT_SELFCHECK_FAIL : (free_bytes < (1024ull << 20) ?
                BONGO_CAT_SELFCHECK_WARN : BONGO_CAT_SELFCHECK_PASS);
        bongo_cat_selfcheck_add(builder, status, "磁盘剩余空间",
            "%.1f GB 可用 / 共 %.1f GB（%s）", free_gb, total_gb, root);
    } else {
        bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_WARN,
            "磁盘剩余空间", "无法读取存储根所在卷的剩余空间（%s）",
            root[0] ? root : "存储根未解析");
    }
#else
    bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_WARN,
        "磁盘剩余空间", "当前平台暂不支持查询剩余空间（仅 Windows 构建提供）");
#endif
}
