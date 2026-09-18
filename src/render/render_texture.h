#ifndef BONGO_CAT_RENDER_TEXTURE_H
#define BONGO_CAT_RENDER_TEXTURE_H

#include "bongo_cat/common.h"
#include "bongo_cat/image.h"

/*
 * 后端无关的纹理句柄。
 *
 * 主窗口的绘制内容 (叠加层、鼠标指针) 需要同时在 OpenGL 与 Vulkan 两条路上
 * 工作。纹理创建/释放集中在这里, 按当前后端分派, 上层模块只持有一个
 * BongoCatTexture, 不再直接出现 GLuint。
 *
 * 只覆盖主窗口这条路径: 设置界面与表盘仍然各自持有 OpenGL 纹理, 它们有自己的
 * GL 上下文, 与这里无关。
 */

typedef struct BongoCatVkTexture BongoCatVkTexture;

typedef struct BongoCatTexture {
    unsigned int gl;            /* OpenGL 纹理名, 0 表示空 */
    BongoCatVkTexture *vk;      /* Vulkan 纹理, NULL 表示空 */
    int width;
    int height;
} BongoCatTexture;

/* 从文件加载 (失败时 error 里是原因) */
bool bongo_cat_texture_from_file(BongoCatTexture *texture, const char *path,
    BongoCatError *error);
/* 从已解码的 RGBA8 像素创建/更新 */
bool bongo_cat_texture_from_image(BongoCatTexture *texture,
    const BongoCatImage *image, bool premultiply, BongoCatError *error);
void bongo_cat_texture_release(BongoCatTexture *texture);
bool bongo_cat_texture_ready(const BongoCatTexture *texture);

#endif
