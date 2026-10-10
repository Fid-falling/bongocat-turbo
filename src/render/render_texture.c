#include "render_texture.h"
#include "bongo_cat/render.h"
#include "bongo_cat/vk_backend.h"

#include <string.h>

/*
 * 先清零再分派: 失败时两个句柄都是空值, 于是调用方用 bongo_cat_texture_ready 统一判断
 * 即可, 不必自己分后端看 gl/vk 哪个字段有效。
 * 分派依据是**当前后端**而不是"哪个字段非空": 两个后端不共享任何纹理, 混用会把 GL 名字
 * 当成 Vulkan 句柄解引用。Vulkan 分支依赖 bongo_cat_vk_current() 非空, 没有活跃上下文
 * 时 (设置窗口/尚未初始化) 加载会失败 —— 这是预期行为, 调用方据此回退。
 */
bool bongo_cat_texture_from_file(BongoCatTexture *texture, const char *path,
    BongoCatError *error) {
    if (!texture || !path) return false;
    memset(texture, 0, sizeof(*texture));
    if (bongo_cat_renderer_active() == BONGO_CAT_RENDERER_VULKAN) {
        texture->vk = bongo_cat_vk_texture_load(bongo_cat_vk_current(), path,
            &texture->width, &texture->height, error);
        return texture->vk != NULL;
    }
    texture->gl = bongo_cat_image_texture(path, &texture->width,
        &texture->height, error);
    return texture->gl != 0;
}

/*
 * 从已解码像素更新纹理。两个后端的行为刻意不同:
 *  - Vulkan 没有原地更新, 只能新建一份再替换: 新纹理建好之后才销毁旧的, 所以失败时
 *    旧纹理完好无损 —— 不变量是"要么保持原样, 要么整体换成新的", 不会留下半旧的纹理;
 *  - GL 复用同一个纹理名原地更新, 省一次分配。
 * premultiply 只在 GL 分支生效: Vulkan 侧像素按原样上传, 预乘由着色器完成 (见
 * overlay.frag 的 rgb *= a), 两条路的最终结果一致。
 */
bool bongo_cat_texture_from_image(BongoCatTexture *texture,
    const BongoCatImage *image, bool premultiply, BongoCatError *error) {
    if (!texture || !image || image->width <= 0 || image->height <= 0) return false;
    if (bongo_cat_renderer_active() == BONGO_CAT_RENDERER_VULKAN) {
        /* 合成结果每次都整块替换: Vulkan 版本重新建一份纹理, 旧的就地释放 */
        BongoCatVkTexture *replacement = bongo_cat_vk_texture_from_pixels(
            bongo_cat_vk_current(), image->pixels, image->width, image->height,
            "overlay-composite", error);
        if (!replacement) return false;
        BongoCatVk *vk = bongo_cat_vk_current();
        if (texture->vk) bongo_cat_vk_texture_destroy(vk, texture->vk);
        texture->vk = replacement;
        texture->width = image->width;
        texture->height = image->height;
        return true;
    }
    texture->gl = bongo_cat_image_upload_texture(image, texture->gl, premultiply);
    texture->width = image->width;
    texture->height = image->height;
    return texture->gl != 0;
}

/* 两个句柄都判一遍而不是"按当前后端只释放一个": 后端切换/重建的中间状态下可能两个
   都有值, 只放一个就会漏。Vulkan 的销毁是**延迟**的 (进 retired 队列等帧栅栏), 所以本
   函数返回后 GPU 可能还在采样这块显存 —— 调用方不要立刻覆写对应的像素数据。 */
void bongo_cat_texture_release(BongoCatTexture *texture) {
    if (!texture) return;
    if (texture->vk) {
        bongo_cat_vk_texture_destroy(bongo_cat_vk_current(), texture->vk);
        texture->vk = NULL;
    }
    if (texture->gl) {
        /* OpenGL 纹理的删除必须发生在当前上下文里, 调用方负责保证这一点 */
        bongo_cat_image_release_texture(texture->gl);
        texture->gl = 0;
    }
    texture->width = texture->height = 0;
}

/* "有纹理可用"的统一判断 (两个后端任一非空)。别用 gl/vk 字段直接判断 —— 那会把
   后端细节漏到调用点, 也就失去了这一层的意义 */
bool bongo_cat_texture_ready(const BongoCatTexture *texture) {
    return texture && (texture->vk != NULL || texture->gl != 0);
}
