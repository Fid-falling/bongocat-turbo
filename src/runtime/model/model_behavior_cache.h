/* 行为目录缓存的两个操作: matches 判断能否直接复用 app->behavior_cache, store 把当前
 * app->behaviors 连同模型 id 与内容摘要存下来; 分配失败或模型不可缓存时只清 valid 位, 不报错。
 */
#ifndef BONGO_CAT_MODEL_BEHAVIOR_CACHE_H
#define BONGO_CAT_MODEL_BEHAVIOR_CACHE_H

#include "bongo_cat/app.h"

bool bongo_cat_model_behavior_cache_matches(const BongoCatApp *app,
    const BongoCatModelEntry *entry);
void bongo_cat_model_behavior_cache_store(BongoCatApp *app,
    const BongoCatModelEntry *entry);

#endif
