/*
 * image_* 模块的内部接口: 只给 src/media/image*.c 用, 不对外安装, 也不该被其它模块 include。
 * 公共 API 在 include/bongo_cat/image.h。
 */
#ifndef BONGO_CAT_IMAGE_INTERNAL_H
#define BONGO_CAT_IMAGE_INTERNAL_H

#include "bongo_cat/image.h"

BongoCatResult bongo_cat_image_decode_pixels(const char *path,
    BongoCatImage *image, BongoCatError *error);
BongoCatResult bongo_cat_image_decode_pixels_responsive(const char *path,
    BongoCatImage *image, BongoCatImageProgress progress, void *userdata,
    BongoCatError *error);
void bongo_cat_image_make_alpha_mask_progress(const BongoCatImage *image,
    BongoCatImageAlphaMask *mask, BongoCatImageProgress progress,
    void *userdata);
/* 之所以必须预乘: 等权平均只对预乘像素成立 —— 未预乘时半透明边缘每降一级就更暗。 */
/* Input pixels must use premultiplied RGBA. */
bool bongo_cat_image_upload_mipmaps(const BongoCatImage *image);

#ifdef _WIN32
bool bongo_cat_image_needs_wic_scaling(const char *path, int limit);
bool bongo_cat_image_decode_wic_responsive(const char *path,
    BongoCatImage *image, int max_width, int max_height,
    BongoCatImageProgress progress, void *userdata);
#endif

#endif
