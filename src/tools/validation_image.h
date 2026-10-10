#ifndef BONGO_CAT_VALIDATION_IMAGE_H
#define BONGO_CAT_VALIDATION_IMAGE_H

#include <windows.h>
#include <stddef.h>
#include <stdint.h>

/*
 * 验证工具共用的图像容器: 32 位 BGRA, 每行紧密排列(没有行填充)。
 *
 * 契约: bgra 由加载 / 归一化函数分配, 必须用 bongo_cat_validation_image_free
 * 释放; 表示区域的 rect 用左上原点的像素坐标、左闭右开区间。
 */
typedef struct BongoCatValidationImage {
    int width;
    int height;
    unsigned char *bgra;
} BongoCatValidationImage;

typedef struct BongoCatValidationRect {
    int left;
    int top;
    int right;
    int bottom;
} BongoCatValidationRect;

int bongo_cat_validation_image_load(const wchar_t *path,
    BongoCatValidationImage *image);
int bongo_cat_validation_image_save(const wchar_t *path,
    const BongoCatValidationImage *image, int png);
void bongo_cat_validation_image_free(BongoCatValidationImage *image);
int bongo_cat_validation_image_normalize_file(const wchar_t *path,
    BongoCatValidationRect anchor, BongoCatValidationImage *output);
int bongo_cat_validation_image_draw_label(BongoCatValidationImage *image,
    const wchar_t *label, float x, float y);
int bongo_cat_validation_image_keep_largest(BongoCatValidationImage *image);
int bongo_cat_validation_image_bounds(const BongoCatValidationImage *image,
    BongoCatValidationRect *bounds);

#endif
