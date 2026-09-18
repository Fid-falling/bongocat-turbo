#ifndef BONGO_CAT_IMAGE_H
#define BONGO_CAT_IMAGE_H

#include "bongo_cat/common.h"

typedef struct SDL_Surface SDL_Surface;

typedef struct BongoCatImage {
    unsigned char *pixels;
    int width;
    int height;
    SDL_Surface *surface;
    bool pixels_stbi;
} BongoCatImage;

#define BONGO_CAT_ALPHA_MASK_SIZE 128
typedef struct BongoCatImageAlphaMask {
    unsigned char pixels[BONGO_CAT_ALPHA_MASK_SIZE * BONGO_CAT_ALPHA_MASK_SIZE];
    int width, height;
} BongoCatImageAlphaMask;
typedef void (*BongoCatImageProgress)(void *userdata, float progress);

#ifdef __cplusplus
extern "C" {
#endif

BongoCatResult bongo_cat_image_load(const char *path, BongoCatImage *image, BongoCatError *error);
bool bongo_cat_image_info(const char *path, int *width, int *height);
void bongo_cat_image_free(BongoCatImage *image);
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
