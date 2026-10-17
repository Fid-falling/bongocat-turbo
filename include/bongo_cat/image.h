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
/* 模型图集的工作尺寸上限。图集可能声明 8192px, 而桌宠只画几百像素: 按原尺寸解码要几百 MB
 * 临时内存, 而且等于放弃图集自带的预平均 —— 采样会落在远高于显示分辨率的密度上, 边缘出现
 * 锯齿。GL 与 Vulkan 两条上传路径必须用**同一个**上限, 否则同一个模型在两个后端会拿到不同
 * 分辨率的图集, 画面就对不上 (见 media/image.c 与 live2d/cubism_model_load.cpp)。 */
#define BONGO_CAT_LIVE2D_TEXTURE_LIMIT 2048
/* 模型图集工作尺寸上限的**运行时**取值: 默认等于上面那个宏。
 *
 * 图集被缩到多少, 决定了屏幕上每条边缘有多少源像素 —— 也就是模型自带的艺术字/署名能不能
 * 看清。做成函数而不是到处直接用宏, 是因为它要能被诊断性地抬高 (实验: 确认"糊"到底是源细节
 * 丢了还是采样/栅格化丢了), 而两条上传路径 (media/image.c 的 GL 路径与
 * live2d/cubism_model_load.cpp 的 Vulkan 路径) 必须读到**同一个**数, 否则同一个模型在两个
 * 后端会拿到不同分辨率的图集, 画面就对不上。
 *
 * 为什么是**共用的一份实现** (src/media/image_limit.c) 而不是两处各写一遍: Vulkan 那条
 * 路径在渲染组件 (bongo_live2d.dll) 里, 而组件按纪律**不链接** bongo_cat_core —— 但它与
 * 宿主必须拿到同一个上限。那份实现被两边共同编译, 于是只有一处逻辑, 又不用把宿主的 core
 * 拖进组件。
 *
 * 环境变量 BONGO_CAT_LIVE2D_TEXTURE_LIMIT=<px> 只用于实验/诊断: 取值夹在
 * [BONGO_CAT_LIVE2D_TEXTURE_MIN_LIMIT, BONGO_CAT_LIVE2D_TEXTURE_MAX_LIMIT], 非法值忽略并
 * 保持默认。不设变量时行为与直接用宏完全一致。 */
#define BONGO_CAT_LIVE2D_TEXTURE_MIN_LIMIT 256
#define BONGO_CAT_LIVE2D_TEXTURE_MAX_LIMIT 16384
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
/* 模型图集工作尺寸上限的运行时取值 (默认 BONGO_CAT_LIVE2D_TEXTURE_LIMIT, 见文件头的说明)。
   GL 与 Vulkan 两条路径都必须问它, 于是同一个模型在两个后端拿到同一个分辨率。 */
int bongo_cat_image_model_texture_limit(void);
/* 设置页下发的上限 (0 = 回到默认)。下一个模型负载生效: 图集尺寸只在解码/上传时决定。 */
void bongo_cat_image_set_model_texture_limit(int limit);
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
