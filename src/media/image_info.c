#include "bongo_cat/file.h"
#include "bongo_cat/image.h"

#include <stb_image.h>

/*
 * 只读文件头拿宽高, 不解码像素: 模型列表和缩略图布局只需要尺寸, 为了一个尺寸付整图解码的
 * 代价不值得 (大图集可达几百 MB 临时内存)。
 *
 * 走 bongo_cat_file_open 而不是 fopen, 是为了 UTF-8 路径 (模型目录常含中文/日文)。
 * 返回 false 表示"尺寸不可信" —— 调用方据此改走更稳的 WIC 解码路径 (见 image.c 里
 * bongo_cat_image_needs_wic_scaling 的调用点)。
 */
bool bongo_cat_image_info(const char *path, int *width, int *height) {
    if (width) *width = 0;
    if (height) *height = 0;
    FILE *file = path ? bongo_cat_file_open(path, "rb") : NULL;
    int image_width = 0, image_height = 0, channels = 0;
    bool known = file && stbi_info_from_file(file, &image_width,
        &image_height, &channels) && image_width > 0 && image_height > 0 &&
        channels > 0;
    if (file) fclose(file);
    if (known && width) *width = image_width;
    if (known && height) *height = image_height;
    return known;
}
