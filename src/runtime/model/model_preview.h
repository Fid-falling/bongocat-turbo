#ifndef BONGO_CAT_MODEL_PREVIEW_H
#define BONGO_CAT_MODEL_PREVIEW_H

#include <stdbool.h>
#include <stddef.h>

/*
 * 取一个模型的预览图路径: 优先 resources/cover.png, 没有就退到模型自己的贴图。
 * 用于轮盘封面与设置卡片 —— 没有封面的模型也能显示成"它自己", 而不是一个通用图标。
 * 返回 false 表示这个模型一张能用的图都找不到。
 */
bool bongo_cat_model_preview_path(const char *model_directory, char *output,
    size_t capacity);

#endif
