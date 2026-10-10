#ifndef BONGO_CAT_MVER_POINTER_H
#define BONGO_CAT_MVER_POINTER_H

#include "bongo_cat/common.h"

/*
 * Mver 皮肤的手指/手臂指示器: 数学部分 (本文件, 实现在 src/core/mver_pointer.c)。
 *
 * 这一层刻意不碰任何图形 API: 输入是"鼠标在桌面上的绝对坐标"或"相对位移",
 * 输出是参考空间 (默认 1400x1400, 由模型元数据的 render.referenceWidth/Height
 * 给出) 里的一条手臂折线与一个设备 (鼠标/手柄) 位置。渲染侧
 * (src/render/mver_pointer_overlay_draw.c) 只负责把折线画出来, 两个渲染后端
 * 共用同一份几何 —— 所以修"手指位置不对"时先看这里, 而不是先看渲染代码。
 *
 * 坐标空间的三次换算, 排查问题时按顺序对:
 *   桌面绝对像素 --ratios--> 0..1 比例 --geometry--> 参考像素 --绘制--> NDC
 */

/* 手臂折线的采样点数。核心数学与绘制两侧都用它开数组, 改大只会变慢, 改小会让
   折线出现棱角; 由于 src/core 里对路径做 19 点贝塞尔采样后再重采样到本点数,
   两侧的采样比例是硬编码耦合的 (index/25.0), 不要单独改这一边。 */
#define BONGO_CAT_MVER_ARM_POINT_COUNT 26

/* 一对参考空间坐标 (像素, 原点在左上) */
typedef struct BongoCatMverPoint {
    float x;
    float y;
} BongoCatMverPoint;

/* 来自模型元数据 standardPointer 段的用户可调偏移: 前两个是设备整体偏移,
   后两个是手/手臂相对设备的偏移 */
typedef struct BongoCatMverPointerConfig {
    float offset_x;
    float offset_y;
    float hand_offset_x;
    float hand_offset_y;
} BongoCatMverPointerConfig;

/* 一帧的几何结果: arm 是从肩膀到手的折线 (画填充与描边都靠它),
   device_x/device_y 是鼠标/手柄贴图的左上角 */
typedef struct BongoCatMverPointerGeometry {
    BongoCatMverPoint arm[BONGO_CAT_MVER_ARM_POINT_COUNT];
    float device_x;
    float device_y;
} BongoCatMverPointerGeometry;

/* 投影窗口在桌面绝对坐标下的矩形。这里用 double 而不是 float: 多显示器时
   left/top 可以是负数, DPI 缩放后数值也能到几万, float 的 24 位尾数会在
   这种量级上丢掉像素级精度 (表现为手指在副屏上抖动) */
typedef struct BongoCatMverPointerBounds {
    double left;
    double top;
    double width;
    double height;
} BongoCatMverPointerBounds;

/*
 * 跨帧保留的指针位置。initialized 是必需的: 第一帧没有历史位置, 若直接按相对
 * 位移累加就会从 (0,0) 开始 (表现为指针从桌面左上角飞进来), 所以第一帧无论
 * 什么模式都用绝对坐标打底。
 */
typedef struct BongoCatMverPointerState {
    double x;
    double y;
    bool initialized;
} BongoCatMverPointerState;

#ifdef __cplusplus
extern "C" {
#endif

/* 0..1 比例 -> 参考空间几何 (纯函数, 无副作用, 可以随便重算)。输入的越界比例
   会被钳制, 所以调用方不必自己防越界; 返回 false 只代表几何退化 (除零之类),
   此时绘制侧整层跳过, 不会画出半截手臂。 */
bool bongo_cat_mver_pointer_geometry(float x_ratio, float y_ratio,
    const BongoCatMverPointerConfig *config,
    BongoCatMverPointerGeometry *geometry);
/* 绝对坐标 -> 0..1 比例。越界的点会被钳到边界 (指针贴边而不是消失), 返回 false
   只表示参数本身不可用 (非有限值, 或 bounds 宽高 <= 0), 不表示"指针在窗口外"。 */
bool bongo_cat_mver_pointer_ratios(double x, double y,
    const BongoCatMverPointerBounds *bounds,
    float *x_ratio, float *y_ratio);
/*
 * 指针位置的状态机。use_relative 对应"鼠标被游戏/远程桌面锁住"的场景: 只有相对
 * 位移可用, 累加后必须钳在 bounds 内, 否则指针会一路漂出屏幕再也回不来。
 * 返回 false 表示这一帧的输入不可信, 调用方应当沿用上一帧的显示位置而不是归零。
 */
bool bongo_cat_mver_pointer_update(BongoCatMverPointerState *state,
    double absolute_x, double absolute_y, double relative_x,
    double relative_y, bool use_relative,
    const BongoCatMverPointerBounds *bounds, double *x, double *y);

#ifdef __cplusplus
}
#endif

#endif
