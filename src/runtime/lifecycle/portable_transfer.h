/*
 * 便携模式的数据搬运原语 (实现见 portable_transfer.c)。
 *
 * 单独一个头/实现文件是因为 portable_mode.c 已经贴着单文件 500 行上限
 * (cmake/CheckLines.cmake 按非 ASCII 片段切行, 中文注释一行能顶好几格), 而"怎么判断
 * 便携模式"和"怎么安全地搬文件"本来就是两件事。
 * 这里的函数全部只做一件事: 复制或删除; 失败语义统一为返回 false, 由调用方决定回滚。
 */
#ifndef BONGO_CAT_PORTABLE_TRANSFER_H
#define BONGO_CAT_PORTABLE_TRANSFER_H

#include "bongo_cat/app.h"
#include "storage_paths.h"

/* 六棵树的下标 (与 storage_paths.c 里的名字一一对应)。前四个是"设置与模型", 也是便携
   模式唯一会搬的四棵树; cache/logs 永远不搬 (理由见 portable_mode.c 的文件头注释)。 */
#define PORTABLE_TREE_CONFIG 0
#define PORTABLE_TREE_MODELS 1
#define PORTABLE_TREE_DATA 2
#define PORTABLE_TREE_STATE 3
#define PORTABLE_TREE_CACHE 4
#define PORTABLE_TREE_LOGS 5
#define PORTABLE_TREE_COUNT 6
#define PORTABLE_TREE_MIGRATED 4

/* "根 + 子目录名" -> 那一棵树; 与 storage_paths.c 的 roots_under 同一规则。 */
bool portable_tree_under(char *target, size_t capacity, const char *root,
    size_t index);
/* 便携根的六棵树 (一次性拼好)。 */
bool portable_trees_from_root(const char *root, BongoCatStorageTrees *trees);
/* app 当前实际在用的六棵树。 */
void portable_trees_from_app(const BongoCatApp *app,
    BongoCatStorageTrees *trees);
/* 探针文件判可写: 只读挂载与写保护的 U 盘在 Windows 上权限位看着都是正常的。 */
bool portable_tree_writable(const char *path);
/*
 * 递归复制一棵树。missing_only 为真时只补目标缺失的文件 —— 关掉便携模式时用它把数据
 * 并回默认目录, 默认目录里更新的数据因此绝不会被便携目录里的旧副本盖掉。
 */
bool portable_copy_tree(const char *source, const char *target,
    bool missing_only);
/* 把 from 的前四棵树复制进 to 的同名树。 */
bool portable_migrate(const BongoCatStorageTrees *from,
    const BongoCatStorageTrees *to, bool missing_only);
/* 递归删除 (只用于回滚本次新建出来的便携根)。 */
bool portable_remove_tree(const char *path);

#endif
