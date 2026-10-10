/*
 * 模型子项的封面缩略图 (只对"模型"这个根项, active == 9 有意义)。
 *
 * 加载策略: 每帧最多一张, 且失败也记进 attempted —— 打开菜单时会看到封面一张张出现,
 * 而不是卡住几百毫秒。attempted 兼作"已经试过"的标记, 所以坏图不会被反复重试。
 * 缓存是 Dial 里的 covers[] 数组, 按模型序号索引, 生命周期与菜单相同 (关掉就随菜单一起
 * 销毁), 不做跨次打开复用 —— 菜单不是常驻窗口, 复用的收益不值得引入失效逻辑。
 *
 * 纹理路径分后端: Vulkan 走 bongo_cat_vk_texture_from_pixels + 纹理表登记; GL 走
 * bongo_cat_image_texture_thumbnail。两条路都必须"先按像素尺寸解出图, 再按实际宽高
 * 等比缩放到 44x40 的格子里", 不能直接拉伸。
 */
#include "dial_internal.h"
#include "bongo_cat/vk_backend.h"
#include "bongo_cat/image.h"
#include "bongo_cat/path.h"

/*
 * 每帧推进封面加载 (在绘制之前调用)。
 * pixels 是按"封面在屏幕上的实际像素尺寸 x 1.25 余量"算出来的, 并且封顶 160 ——
 * 高清屏上按 4x 直接解原图会明显卡顿, 而这个尺寸已经比显示区域大, 看不出差别。
 * 结尾那个 return 就是"每帧一张"的实现。
 */
void dial_covers_tick(Dial *d) {
    if (d->active != 9) return;
    for (int i = 0; i < dial_child_count(d); ++i) {
        size_t index = (size_t)(d->page*DIAL_PAGE+i);
        if (index >= d->labels->model_count || index >= BONGO_CAT_MODEL_CAP) continue;
        DialCover *cover = &d->covers[index];
        if (cover->attempted) continue;
        cover->attempted = true;
        const char *directory = d->labels->model_cover_directories ?
            d->labels->model_cover_directories[index] : NULL;
        char path[BONGO_CAT_PATH_CAP];
        if (directory && bongo_cat_path_join(path,sizeof(path),directory,"resources/cover.png")) {
            int width = 0, height = 0;
            BongoCatError ignored = {0};
            int pixels = (int)fminf(160,ceilf(44*d->scale*d->raster_scale*1.25f));
            if (d->vk) {
            /* Vulkan 菜单: 用后端纹理接口创建封面并登记到纹理表 */
            BongoCatImage image = {0};
            if (bongo_cat_image_fit(path, pixels, pixels, &image, &ignored) ==
                    BONGO_CAT_OK && image.pixels) {
                width = image.width;
                height = image.height;
                BongoCatVkTexture *texture = bongo_cat_vk_texture_from_pixels(d->vk,
                    image.pixels, image.width, image.height, path, &ignored);
                bongo_cat_image_free(&image);
                cover->texture = texture
                    ? (unsigned int)dial_vk_texture_register(d, texture) : 0;
            }
        } else {
        cover->texture = bongo_cat_image_texture_thumbnail(path,pixels,pixels,&width,&height,&ignored);
        }
            if (cover->texture && width > 0 && height > 0) {
                float fit = fminf(44.0f/(float)width,40.0f/(float)height);
                cover->width = (float)width*fit; cover->height = (float)height*fit;
            }
        }
        d->dirty = true;
        return; /* One image per frame; failed reads are also cached. */
    }
}

bool dial_cover_draw(Dial *d, int child, float x, float y, float opacity) {
    if (d->active != 9 || child < 0) return false;
/*
 * 画一个子项的封面; 没有封面就退回通用图标。返回 true 表示"这个子项已经画过了",
 * 调用方 (dial_scene) 据此跳过文字 —— 有封面的项不再叠标签, 否则会糊成一团。
 */
    size_t index = (size_t)(d->page*DIAL_PAGE+child);
    if (index >= d->labels->model_count || index >= BONGO_CAT_MODEL_CAP) return false;
    DialCover *cover = &d->covers[index];
    uint32_t color = 0xffffff | ((uint32_t)(255*opacity)<<24);
    if (cover->texture && cover->width > 0 && cover->height > 0)
        dial_quad(d,cover->texture,x-cover->width/2,y-cover->height/2,
            cover->width,cover->height,0,0,1,1,color);
    else dial_icon(d,9,x,y,28,(color&0xff000000)|0xa78bfa);
    return true;
}
