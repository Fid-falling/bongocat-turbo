/*
 * 便携模式: 把六棵存储树从"平台默认目录"换成"程序目录下的 BongoCatData"。
 *
 * 判定只有一条: <exe 目录>\BongoCatData\portable.json 这个标记文件在不在。
 * 为什么不把开关写进 settings.json: 那是个先有鸡还是先有蛋的问题 —— 要读 settings
 * 得先知道它在哪。标记放在便携根里, 于是"exe + BongoCatData 整体拷走"仍然是便携模式
 * (绿色迁移), 而删掉标记就干净地回到默认目录, 不需要第二个真相来源。
 *
 * 与 --storage-root 的关系: 命令行参数优先级更高 (见 storage_paths.c 的解析顺序)。
 * 便携模式本质就是给存储根换一个位置, 所以复用的还是 trees_under 那一套解析, 这里
 * 只负责"根在哪"以及"数据怎么搬过去/搬回来"。
 *
 * 迁移只搬设置与模型 (config/ models/ data/ state/), 不搬 cache/ 与 logs/:
 *   - cache 是几百 MB 的可重建数据, 为它做一次全量拷贝毫无意义, 让它按需重建;
 *   - logs 是当前进程正开着的文件, 复制它既没意义 (内容还在长) 也可能被锁。
 * 具体的复制/删除原语在 portable_transfer.c (那份文件只管搬运, 不管策略)。
 *
 * 两个方向的失败语义刻意不同:
 *   - 开启失败: 回滚本次新建的便携根, 标记不写 —— 下一次启动仍然用默认目录;
 *   - 关闭失败: 标记不删, 便携模式保持开启。宁可不动, 也不要停在"数据搬了一半、两边
 *     都不完整"的状态上。
 */
#include "portable_mode.h"
#include "portable_transfer.h"

#include "bongo_cat/file.h"
#include "bongo_cat/log.h"
#include "bongo_cat/path.h"

#include <SDL3/SDL.h>
#include <stdio.h>

#define BONGO_CAT_PORTABLE_DIRECTORY "BongoCatData"
#define BONGO_CAT_PORTABLE_MARKER "portable.json"

/* 标记文件的内容只是给人看的 (程序只看它在不在), 所以写成自带说明的一段 JSON。 */
static const char marker_content[] =
    "{\n"
    "  \"portable\": true,\n"
    "  \"note\": \"BongoCat portable mode. All application data lives in this "
    "folder; copy the executable together with this folder to migrate. "
    "Delete this file to go back to the default location.\"\n"
    "}\n";

bool bongo_cat_portable_mode_root(char *output, size_t capacity) {
    if (!output || !capacity) return false;
    output[0] = '\0';
    /* SDL_GetBasePath 给的是"程序所在目录 + 分隔符", 且是 UTF-8 (与 path.h 一致)。
       拿不到就直接失败, 不退回当前工作目录 —— 便携模式的语义是"跟着 exe 走",
       用工作目录猜出来的根会让用户的数据落在一个他想不到的地方。
       注意这块内存由 SDL 自己缓存 (SDL_filesystem.c 的 CachedBasePath), 不能 free。 */
    const char *base = SDL_GetBasePath();
    return base && base[0] && bongo_cat_path_join(output, capacity, base,
        BONGO_CAT_PORTABLE_DIRECTORY);
}

static bool marker_path(char *output, size_t capacity) {
    char root[BONGO_CAT_PATH_CAP];
    return bongo_cat_portable_mode_root(root, sizeof(root)) &&
        bongo_cat_path_join(output, capacity, root, BONGO_CAT_PORTABLE_MARKER);
}

bool bongo_cat_portable_mode_active(void) {
    char marker[BONGO_CAT_PATH_CAP];
    return marker_path(marker, sizeof(marker)) &&
        bongo_cat_path_is_file(marker);
}

/* 六棵树全部可写才敢写标记: 标记一旦落地, 下一次启动就会按便携根解析全部存储,
   那时任何一棵树不可写都会让启动直接失败 (storage_paths.c 不做部分降级)。 */
static bool prepare_portable_trees(const char *root, BongoCatError *error) {
    for (size_t i = 0; i < PORTABLE_TREE_COUNT; ++i) {
        char child[BONGO_CAT_PATH_CAP] = {0};
        if (!portable_tree_under(child, sizeof(child), root, i) ||
            !portable_tree_writable(child)) {
            bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
                "Portable folder is not writable: %s",
                child[0] ? child : root);
            return false;
        }
    }
    return true;
}

/* 只在"本次真的建出了这个目录"时才回滚。目标根原本就存在时绝不能整个删掉 —— 那会删掉
   用户自己放在那里的东西; 同名普通文件同理 (prepare 阶段就会失败, 更不能顺手删了它)。 */
static void rollback_portable_root(const char *root, bool created) {
    if (!created || !bongo_cat_path_is_dir(root)) return;
    if (portable_remove_tree(root))
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[portable] 迁移失败, 已回滚新建的便携目录: %s", root);
    else
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[portable] 迁移失败, 便携目录未能完全回滚 (可手动删除): %s", root);
}

/* 写标记。必须最后做: 中途失败时它不存在, 下一次启动仍然用默认目录。 */
static bool write_marker(const char *marker) {
    FILE *file = bongo_cat_file_open(marker, "wb");
    size_t length = sizeof(marker_content) - 1;
    bool written = file &&
        fwrite(marker_content, 1, length, file) == length;
    if (file && fclose(file) != 0) written = false;
    return written;
}

BongoCatResult bongo_cat_portable_mode_enable(const BongoCatApp *app,
    BongoCatError *error) {
    if (!app) return BONGO_CAT_ERROR_ARGUMENT;
    if (bongo_cat_portable_mode_active()) return BONGO_CAT_OK;
    char root[BONGO_CAT_PATH_CAP], marker[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_portable_mode_root(root, sizeof(root)) ||
        !marker_path(marker, sizeof(marker))) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Cannot locate the folder next to the executable");
        return BONGO_CAT_ERROR_PLATFORM;
    }
    bool created = !bongo_cat_path_is_dir(root);
    if (!prepare_portable_trees(root, error)) {
        rollback_portable_root(root, created);
        return BONGO_CAT_ERROR_IO;
    }
    BongoCatStorageTrees portable = {0}, current = {0};
    portable_trees_from_app(app, &current);
    /* 开启方向以"当前实际在用的目录"为准, 所以允许覆盖: 用户正在用的那份才最新。 */
    if (!portable_trees_from_root(root, &portable) ||
        !portable_migrate(&current, &portable, false)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Cannot copy settings and models into the portable folder: %s", root);
        rollback_portable_root(root, created);
        return BONGO_CAT_ERROR_IO;
    }
    if (!write_marker(marker)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Cannot write the portable mode marker: %s", marker);
        rollback_portable_root(root, created);
        return BONGO_CAT_ERROR_IO;
    }
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[portable] 便携模式已开启: %s (设置与模型已复制, 缓存不搬、按需重建)", root);
    return BONGO_CAT_OK;
}

BongoCatResult bongo_cat_portable_mode_disable(const BongoCatApp *app,
    BongoCatError *error) {
    if (!app) return BONGO_CAT_ERROR_ARGUMENT;
    char root[BONGO_CAT_PATH_CAP], marker[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_portable_mode_root(root, sizeof(root)) ||
        !marker_path(marker, sizeof(marker)) ||
        !bongo_cat_path_is_file(marker)) return BONGO_CAT_OK;
    /*
     * 合并目标 = 关掉便携模式以后程序会真正使用的目录:
     *   - 带了 --storage-root= 时就是它 (命令行优先级最高);
     *   - 否则是平台默认目录 —— 此时 app->*_root 指的正是便携目录本身, 拿它当目标
     *     等于把文件复制到自己身上。
     * 断言一次"目标不是便携目录自己": 自动重启会把便携目录写成 --storage-root
     * (见 application_restart.c), 于是"当前存储根"与"便携根"可能是同一个 —— 那时
     * 唯一的正确目标只能是平台默认目录, 否则数据会留在 BongoCatData 里而用户以为
     * 已经搬回来了。
     */
    BongoCatStorageTrees portable = {0}, target = {0};
    if (!portable_trees_from_root(root, &portable)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Cannot resolve the portable folder: %s", root);
        return BONGO_CAT_ERROR_PLATFORM;
    }
    bool target_is_portable = false;
    if (app->storage_root[0]) {
        portable_trees_from_app(app, &target);
        target_is_portable = strcmp(target.config, portable.config) == 0;
    }
    if (!app->storage_root[0] || target_is_portable) {
        if (!bongo_cat_storage_paths_platform_trees(&target)) {
            bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
                "Cannot resolve the default storage folders to merge back into");
            return BONGO_CAT_ERROR_IO;
        }
    }
    /* missing_only: 只补默认目录里缺的文件, 绝不覆盖那里更新的数据。 */
    if (!portable_migrate(&portable, &target, true)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Cannot merge the portable data back; portable mode stays enabled");
        return BONGO_CAT_ERROR_IO;
    }
    /* 标记删掉 = 便携模式关闭。目录本身留着 (里面有当前进程正开着的日志), 数据都已经
       并回默认目录, 由用户自己决定什么时候删。 */
    if (!bongo_cat_path_remove(marker)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Cannot remove the portable mode marker: %s", marker);
        return BONGO_CAT_ERROR_IO;
    }
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[portable] 便携模式已关闭, 缺失的设置与模型已并回默认目录; "
        "残留的缓存/日志目录可以手动删除: %s", root);
    return BONGO_CAT_OK;
}
