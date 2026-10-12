/*
 * 便携模式的内部接口 (实现见 portable_mode.c)。
 *
 * 只有两件事对外: "便携模式开着吗 / 便携根在哪" 和 "开启 / 关闭并搬数据"。
 * 判定与迁移都必须走这里, 不要在各处自己拼 <exe>\BongoCatData —— 那就是第二套解析,
 * 一定会和 storage_paths.c 里的那套对不上。
 */
#ifndef BONGO_CAT_PORTABLE_MODE_H
#define BONGO_CAT_PORTABLE_MODE_H

#include "bongo_cat/app.h"

/* 便携根 = <exe 目录>\BongoCatData。目录不存在也返回 true (它只是个位置,
   是否启用由标记文件决定)。取不到 exe 目录时返回 false。 */
bool bongo_cat_portable_mode_root(char *output, size_t capacity);
/* 标记文件 <便携根>\portable.json 存在 = 便携模式开启。 */
bool bongo_cat_portable_mode_active(void);
/* 开启: 六棵树全部可写才动手, 把 config/models/data/state 复制进便携根, 最后写标记。
   任何一步失败都回滚本次新建的便携根并返回错误 —— 绝不留下"标记在、数据不全"。 */
BongoCatResult bongo_cat_portable_mode_enable(const BongoCatApp *app,
    BongoCatError *error);
/* 关闭: 把便携根里的设置与模型按"目标缺失才复制"并回关掉便携模式后会用的目录,
   成功后才删标记。合并失败时保持开启 (宁可不动, 也不要半途丢掉数据)。 */
BongoCatResult bongo_cat_portable_mode_disable(const BongoCatApp *app,
    BongoCatError *error);

#endif
