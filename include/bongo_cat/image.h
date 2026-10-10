#ifndef BONGO_CAT_IMAGE_H
#define BONGO_CAT_IMAGE_H

#include "bongo_cat/common.h"

typedef struct SDL_Surface SDL_Surface;

/*
 * 一张解码完成的 RGBA 图。pixels 与 surface 都归本结构所有, 一律用 bongo_cat_image_free()
 * 释放 (它按 pixels_stbi 选对应的分配器 —— 两个分配器不能混用); surface 只是给 SDL 缩放/保存
 * 用的视图, 不拥有像素内存。
 */
typedef struct BongoCatImage {
    unsigned char *pixels;
    int width;
    int height;
    SDL_Surface *surface;
    bool pixels_stbi;
} BongoCatImage;

/* 降采样 alpha 掩码的边长上限 (不是纹理尺寸): 供 CPU 侧判断"这块有没有可见像素", 于是扫描
 * 三角形时不必回读原图。128 足够做"可见/不可见"这种粗判。 */
#define BONGO_CAT_ALPHA_MASK_SIZE 128
typedef struct BongoCatImageAlphaMask {
    unsigned char pixels[BONGO_CAT_ALPHA_MASK_SIZE * BONGO_CAT_ALPHA_MASK_SIZE];
    int width, height;
} BongoCatImageAlphaMask;
typedef void (*BongoCatImageProgress)(void *userdata, float progress);

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 分层约定: 凡是返回 BongoCatResult / 填 BongoCatImage 的接口 (load/fit/composite/alpha mask)
 * 都是纯 CPU, 不需要 GL 上下文, 所以 Vulkan 后端也能复用; 凡是返回 unsigned int 纹理名的
 * 接口都要求调用线程当前有可用的 GL 上下文, Vulkan 路径改走 vk_backend 的纹理接口。
 */
BongoCatResult bongo_cat_image_load(const char *path, BongoCatImage *image, BongoCatError *error);
bool bongo_cat_image_info(const char *path, int *width, int *height);
void bongo_cat_image_free(BongoCatImage *image);
/* 只做 CPU 处理: 载入图片, 超过上限时等比缩小 (不涉及任何 GPU 资源)。
   Vulkan 的界面纹理用它, OpenGL 侧继续用下面返回纹理名的接口。 */
BongoCatResult bongo_cat_image_fit(const char *path, int max_width,
    int max_height, BongoCatImage *out, BongoCatError *error);
unsigned int bongo_cat_image_texture(const char *path, int *width, int *height,
    BongoCatError *error);
/* 把已解码的像素上传到 texture (0 表示新建)。premultiply 为真时就地预乘 alpha。
   返回纹理名。Vulkan 后端用不到, 走 vk_backend 的纹理接口。 */
unsigned int bongo_cat_image_upload_texture(const BongoCatImage *image,
    unsigned int texture, bool premultiply);
void bongo_cat_image_release_texture(unsigned int texture);
/* 只做 CPU 合成 (基图 + 左右按键图, 可选擦除脚印), 不涉及任何 GPU 资源 */
BongoCatResult bongo_cat_image_composite(const char *base, const char *left,
    const char *right, bool erase_left, bool erase_right, BongoCatImage *out,
    BongoCatError *error);
unsigned int bongo_cat_image_texture_thumbnail(const char *path, int max_width,
    int max_height, int *width, int *height, BongoCatError *error);
unsigned int bongo_cat_image_texture_resampled(const char *path,
    int max_width, int max_height, float rounding, int *width, int *height,
    BongoCatError *error);
/* Returns a premultiplied-alpha texture for the Cubism renderer. */
unsigned int bongo_cat_image_texture_model(const char *path, bool direct_decode,
    int *width, int *height, BongoCatImageAlphaMask *alpha,
    BongoCatImageProgress progress, void *userdata, BongoCatError *error);
void bongo_cat_image_make_alpha_mask(const BongoCatImage *image,
    BongoCatImageAlphaMask *mask);
unsigned int bongo_cat_image_composite_texture(const char *base, const char *left,
    const char *right, unsigned int texture, bool erase_left, bool erase_right,
    BongoCatError *error);

#ifdef __cplusplus
}
#endif

#endif
