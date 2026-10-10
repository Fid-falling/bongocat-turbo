#ifndef BONGO_CAT_MVER_POINTER_OVERLAY_INTERNAL_H
#define BONGO_CAT_MVER_POINTER_OVERLAY_INTERNAL_H

#include "mver_pointer_overlay.h"
#include "bongo_cat/gl_api.h"
#include "bongo_cat/mver_pointer.h"
#include "bongo_cat/render.h"
#include "render_texture.h"

/* 后端无关的纹理句柄 + 尺寸缓存。尺寸在 load 时抄一份, 是因为绘制路径要按
   "贴图像素 * scale" 算四边形, 而尺寸查询在 Vulkan 侧要过一遍句柄解引用,
   每帧每层都查不划算, 也避免纹理被延迟销毁时读到空句柄。 */
typedef struct BongoCatPointerTexture {
    BongoCatTexture texture;
    int width;
    int height;
} BongoCatPointerTexture;

/*
 * GL 资源 (gl/program/vao/vbo) 在 Vulkan 后端下**全是 0**: create 直接早退, 既不
 * 加载入口点也不建程序。因此这里的所有 GL 调用都必须以"对象名非 0"为前提 (见
 * destroy 里的 if 守卫), 否则在 Vulkan 路径下会调用空函数指针当场崩溃。
 *
 * textured_location/image_location 是 uniform 位置, 与 program 同生命周期: 因为
 * program 只创建一次 (不像纹理那样会重新加载), 所以只在 create 时查询一次。
 *
 * x_ratio/y_ratio 是"本帧待绘制"的比例, 由 set() 每帧写入, 绘制时才换算成几何;
 * 中间不缓存几何结果, 是因为一次 begin/end frame 之间可能被 before/after 两次
 * 调用, 各自算一次比维护脏标记更简单。
 */
struct BongoCatMverPointerOverlay {
    BongoCatGL gl;
    unsigned int program;
    int textured_location;
    int image_location;
    unsigned int vao;
    unsigned int vbo;
    BongoCatPointerTexture arm;
    BongoCatPointerTexture device;
    BongoCatPointerTexture left;
    BongoCatPointerTexture right;
    BongoCatPointerTexture side;
    /* 来自元数据的用户偏移 (offsetX/offsetY/handOffsetX/handOffsetY) */
    BongoCatMverPointerConfig geometry;
    /* 几何所在的参考分辨率 (元数据 render.referenceWidth/Height, 缺省 1400x1400)。
       它同时是 NDC 换算的分母, 所以为 0 时直接判定加载失败, 不允许 enabled。 */
    int reference_width;
    int reference_height;
    float scale;
    float x_ratio;
    float y_ratio;
    /* 手臂描边的颜色, 元数据给的是 0..255, 这里已经归一化 */
    float line_red;
    float line_green;
    float line_blue;
    /* 资源齐全且元数据要求启用 —— 唯一的绘制开关, 也是所有绘制函数的入口判断 */
    bool enabled;
    /* true = 设备图跟随系统鼠标 (额外叠加鼠标贴图); false = 画手/手柄, 由按键换贴图 */
    bool mouse;
    bool left_handed;
    /* 三个按键的当前状态, 由输入层每帧通过 set() 写入 */
    bool left_down;
    bool right_down;
    bool side_down;
};

#endif
