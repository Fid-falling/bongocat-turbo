/*
 * 便携模式的数据搬运原语: 递归复制、只补缺失的合并、递归删除 (回滚用)。
 *
 * 三条不变量:
 * 1. 只复制、从不移动。源目录因此在任何失败下都是完整的, "两边都不完整"这种状态根本
 *    不可能出现 —— 最坏情况只是目标侧少几个文件;
 * 2. 复制失败必须让整棵树失败。静默跳过会让用户以为数据都搬完了, 那比直接报错更糟;
 * 3. missing_only 的那条路 (关便携模式时合并回默认目录) 只看"目标在不在", 不比较时间
 *    —— 默认目录里的数据永远优先, 便携目录只是补它缺的那些。
 */
#include "portable_transfer.h"

#include "bongo_cat/file.h"
#include "bongo_cat/log.h"
#include "bongo_cat/path.h"

#include <stdio.h>

static const char *tree_name(size_t index) {
    switch (index) {
    case PORTABLE_TREE_CONFIG: return "config";
    case PORTABLE_TREE_MODELS: return "models";
    case PORTABLE_TREE_DATA: return "data";
    case PORTABLE_TREE_STATE: return "state";
    case PORTABLE_TREE_CACHE: return "cache";
    default: return "logs";
    }
}

/* 六棵树的统一取法。返回 char* 而不是 const char*: 便携根的六棵树就是靠"往格子里拼
   路径"填出来的 (见 portable_trees_from_root)。 */
static char *tree_slot(BongoCatStorageTrees *trees, size_t index) {
    switch (index) {
    case PORTABLE_TREE_CONFIG: return trees->config;
    case PORTABLE_TREE_MODELS: return trees->models;
    case PORTABLE_TREE_DATA: return trees->data;
    case PORTABLE_TREE_STATE: return trees->state;
    case PORTABLE_TREE_CACHE: return trees->cache;
    default: return trees->logs;
    }
}

bool portable_tree_under(char *target, size_t capacity, const char *root,
    size_t index) {
    return root && root[0] && bongo_cat_path_join(target, capacity, root,
        tree_name(index));
}

bool portable_trees_from_root(const char *root, BongoCatStorageTrees *trees) {
    if (!trees) return false;
    for (size_t i = 0; i < PORTABLE_TREE_COUNT; ++i)
        if (!portable_tree_under(tree_slot(trees, i), BONGO_CAT_PATH_CAP, root, i))
            return false;
    return true;
}

void portable_trees_from_app(const BongoCatApp *app,
    BongoCatStorageTrees *trees) {
    if (!app || !trees) return;
    snprintf(trees->config, sizeof(trees->config), "%s", app->config_root);
    snprintf(trees->data, sizeof(trees->data), "%s", app->data_root);
    snprintf(trees->models, sizeof(trees->models), "%s", app->models_root);
    snprintf(trees->cache, sizeof(trees->cache), "%s", app->cache_root);
    snprintf(trees->state, sizeof(trees->state), "%s", app->state_root);
    snprintf(trees->logs, sizeof(trees->logs), "%s", app->log_root);
}

bool portable_tree_writable(const char *path) {
    char probe[BONGO_CAT_PATH_CAP];
    if (!path || !path[0] || !bongo_cat_path_create_directory(path) ||
        !bongo_cat_path_join(probe, sizeof(probe), path,
            ".bongocat-write-probe")) return false;
    FILE *file = bongo_cat_file_open(probe, "wb");
    bool written = file && fputc('1', file) != EOF;
    if (file && fclose(file) != 0) written = false;
    bongo_cat_file_remove(probe);
    return written;
}

typedef struct CopyVisit {
    const char *target;
    bool missing_only;
    bool ok;
    unsigned files;
} CopyVisit;

static BongoCatPathVisit copy_visit(void *userdata, const char *directory,
    const char *name) {
    CopyVisit *visit = userdata;
    char source[BONGO_CAT_PATH_CAP], target[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(source, sizeof(source), directory, name) ||
        !bongo_cat_path_join(target, sizeof(target), visit->target, name)) {
        visit->ok = false;
        return BONGO_CAT_PATH_FAILURE;
    }
    if (bongo_cat_path_is_dir(source)) {
        if (!portable_copy_tree(source, target, visit->missing_only)) {
            visit->ok = false;
            return BONGO_CAT_PATH_FAILURE;
        }
        return BONGO_CAT_PATH_CONTINUE;
    }
    if (visit->missing_only && bongo_cat_path_is_file(target))
        return BONGO_CAT_PATH_CONTINUE;
    if (!bongo_cat_path_copy_file(source, target)) {
        visit->ok = false;
        return BONGO_CAT_PATH_FAILURE;
    }
    visit->files++;
    return BONGO_CAT_PATH_CONTINUE;
}

bool portable_copy_tree(const char *source, const char *target,
    bool missing_only) {
    if (!source || !source[0]) return true;
    if (!bongo_cat_path_is_dir(source)) return true; /* 源里没有这棵树: 没什么可搬 */
    if (!bongo_cat_path_create_directory(target)) return false;
    CopyVisit visit = {target, missing_only, true, 0};
    if (!bongo_cat_path_enumerate(source, copy_visit, &visit) || !visit.ok)
        return false;
    if (visit.files)
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[portable] 已复制 %u 个文件: %s -> %s", visit.files, source, target);
    return true;
}

bool portable_migrate(const BongoCatStorageTrees *from,
    const BongoCatStorageTrees *to, bool missing_only) {
    if (!from || !to) return false;
    bool ok = true;
    for (size_t i = 0; ok && i < PORTABLE_TREE_MIGRATED; ++i)
        ok = portable_copy_tree(tree_slot((BongoCatStorageTrees *)from, i),
            tree_slot((BongoCatStorageTrees *)to, i), missing_only);
    return ok;
}

typedef struct RemoveVisit {
    bool ok;
} RemoveVisit;

static BongoCatPathVisit remove_visit(void *userdata, const char *directory,
    const char *name) {
    RemoveVisit *visit = userdata;
    char path[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(path, sizeof(path), directory, name) ||
        !bongo_cat_path_remove(path)) {
        visit->ok = false;
        return BONGO_CAT_PATH_FAILURE;
    }
    return BONGO_CAT_PATH_CONTINUE;
}

bool portable_remove_tree(const char *path) {
    if (!path || !path[0]) return false;
    if (!bongo_cat_path_is_dir(path)) return bongo_cat_path_remove(path);
    RemoveVisit visit = {true};
    if (!bongo_cat_path_enumerate(path, remove_visit, &visit) || !visit.ok)
        return false;
    return bongo_cat_path_remove(path);
}
