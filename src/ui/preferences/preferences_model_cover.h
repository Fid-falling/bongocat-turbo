#ifndef BONGO_CAT_PREFERENCES_MODEL_COVER_H
#define BONGO_CAT_PREFERENCES_MODEL_COVER_H

#include "bongo_cat/model.h"

typedef struct BongoCatApp BongoCatApp;

/*
 * 模型封面缓存 (实现在 preferences_model_cover.c)。
 * 使用节奏是固定三步, 顺序不能变:
 * covers_begin -> 画卡片时对每张卡调 cover() 取图 -> covers_prune
 * 返回的指针只在"本帧 begin/prune 之间"有效: prune 会回收本帧没被取过的槽位, 所以
 * 拿到指针后不要跨帧保存。请求尺寸按物理像素给 (卡片会乘 raster_scale)。
 */
typedef struct BongoCatModelCover {
    unsigned int texture;
    int width;
    int height;
} BongoCatModelCover;

void bongo_cat_preferences_model_covers_begin(BongoCatApp *app);
void bongo_cat_preferences_model_cover_cache_clear(BongoCatApp *app);
bool bongo_cat_preferences_model_cover_reload(BongoCatApp *app,
    const char *path);
void bongo_cat_preferences_model_cache_abandon(BongoCatApp *app);
const BongoCatModelCover *bongo_cat_preferences_model_cover(
    BongoCatApp *app, const BongoCatModelEntry *entry,
    int pixel_width, int pixel_height);
void bongo_cat_preferences_model_covers_prune(BongoCatApp *app);

#endif
