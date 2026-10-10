/* 模型切换几何适配的内部接口 (仅供 runtime 内部): anchor 必须在切换前取,
 * apply_aspect 必须在渲染选项与 live2d 尺寸都就绪后调; 返回 true 表示窗口几何真的改了。
 */
#ifndef BONGO_CAT_MODEL_GEOMETRY_H
#define BONGO_CAT_MODEL_GEOMETRY_H

#include "runtime.h"

typedef struct BongoCatModelContentAnchor {
    int x, y;
    bool valid;
} BongoCatModelContentAnchor;

BongoCatModelContentAnchor bongo_cat_model_content_anchor(BongoCatApp *app);
bool bongo_cat_model_apply_aspect(BongoCatApp *app,
    const BongoCatLive2DRenderOptions *options,
    const BongoCatModelContentAnchor *anchor, bool replacing_model);

#endif
