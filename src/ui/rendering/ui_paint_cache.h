#ifndef BONGO_CAT_UI_PAINT_CACHE_H
#define BONGO_CAT_UI_PAINT_CACHE_H

#include "ui_backend.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * 每种效果的 key 必须能唯一决定烘焙结果 —— 只要有一个输入没进 key, 改了它就会
 * 命中旧纹理 (主题/尺寸变化时表现为"画的是上一套颜色")。
 * 颜色一律 pack() 成 uint32 塞进 key; 浮点参数则先乘系数再 lroundf 量化,
 * 这样 1e-4 的抖动不会导致缓存全部失效。
 */
typedef enum BongoCatUIPaintKind {
    BONGO_CAT_UI_PAINT_GRADIENT = 1,
    BONGO_CAT_UI_PAINT_RADIAL = 2,
    BONGO_CAT_UI_PAINT_SHADOW = 3,
    BONGO_CAT_UI_PAINT_RADIAL_CIRCLE = 4,
    BONGO_CAT_UI_PAINT_DASHED_ROUNDED = 5,
    BONGO_CAT_UI_PAINT_SIDEBAR_GLOW = 6,
    BONGO_CAT_UI_PAINT_ROUNDED_SURFACE = 7
} BongoCatUIPaintKind;

/* 逐字节 memcmp 比较, 所以调用方必须把结构体整体初始化 (用聚合初始化或 memset),
   留下未赋值的填充/字段会让"同样的参数"每次算出不同的 key, 缓存永不命中。 */
typedef struct BongoCatUIPaintKey {
    int kind, width, height, radius;
    int first_parameter, second_parameter;
    uint32_t first_color, second_color;
} BongoCatUIPaintKey;

typedef struct BongoCatUIPaintTexture BongoCatUIPaintTexture;

BongoCatUIPaintTexture *bongo_cat_ui_paint_cache_get(
    BongoCatUIBackend *backend, const BongoCatUIPaintKey *key);
/* texture != 0 才算烘焙完成。false 时调用方要自己填 pixels 并调 upload()。 */
bool bongo_cat_ui_paint_cache_ready(
    const BongoCatUIPaintTexture *item);
/* single_channel: 只上传 8 位 alpha (模糊/发光类), 显存与带宽是 RGBA 的 1/4,
   代价是颜色信息丢失 —— 绘制时必须用 tint 把 RGB 补回来。 */
bool bongo_cat_ui_paint_cache_upload(BongoCatUIPaintTexture *item,
    const unsigned char *pixels, bool single_channel);
void bongo_cat_ui_paint_cache_draw(struct nk_context *context,
    struct nk_rect bounds, const BongoCatUIPaintTexture *item,
    struct nk_color tint);
/* 每帧一次, 必须在任何 get() 之前 (见 ui_paint_lifecycle.c 的时序说明) */
void bongo_cat_ui_paint_cache_begin_frame(BongoCatUIBackend *backend);
/* 诊断用: 当前 backend 占用的字节数与条目数 */
size_t bongo_cat_ui_paint_cache_usage(BongoCatUIBackend *backend,
    size_t *texture_count);
/* 真正释放 GPU 资源; 设备/上下文已经没了的时候改用 abandon() */
void bongo_cat_ui_paint_cache_destroy(BongoCatUIBackend *backend);
void bongo_cat_ui_paint_cache_abandon(BongoCatUIBackend *backend);

#endif
