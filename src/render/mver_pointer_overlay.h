#ifndef BONGO_CAT_MVER_POINTER_OVERLAY_H
#define BONGO_CAT_MVER_POINTER_OVERLAY_H

#include "bongo_cat/common.h"

/*
 * Mver 皮肤指针/手臂覆盖层 (不透明句柄)。它由 overlay 拥有: overlay 在 create 时
 * 连带创建它, 在 destroy/clear 时连带销毁/清空, 其它模块不直接持有指针。
 *
 * 重要: 句柄非空不代表这一层可用。Vulkan 后端下不创建任何 GL 资源, create 返回的
 * 是一个空壳 (enabled 为 false), 所有函数都必须能安全地作用在空壳上。所以调用方
 * 一律用 bongo_cat_mver_pointer_overlay_enabled() 判断"要不要画/要不要喂数据",
 * 而不是判断指针非空。
 */
typedef struct BongoCatMverPointerOverlay BongoCatMverPointerOverlay;

BongoCatMverPointerOverlay *bongo_cat_mver_pointer_overlay_create(
    BongoCatError *error);
void bongo_cat_mver_pointer_overlay_destroy(BongoCatMverPointerOverlay *value);
void bongo_cat_mver_pointer_overlay_clear(BongoCatMverPointerOverlay *value);
/*
 * 按模型目录里的元数据装载 (未启用时什么都不做)。返回 true 且 enabled() 为 false
 * 是**正常结果**: 表示这个模型没有开启 Mver 指针功能, 调用方不要当成错误报给用户。
 * 返回 false 才是真的失败 (元数据读不了/必需贴图缺失), 这时内部已经清空, 不会留下
 * 半套纹理让后面画出残缺的手臂。
 */
bool bongo_cat_mver_pointer_overlay_load(BongoCatMverPointerOverlay *value,
    const char *directory, BongoCatError *error);
bool bongo_cat_mver_pointer_overlay_enabled(
    const BongoCatMverPointerOverlay *value);
bool bongo_cat_mver_pointer_overlay_left_handed(
    const BongoCatMverPointerOverlay *value);
/* 每帧喂入指针比例与三个按键状态; 未启用时静默忽略 (调用方不必先判断) */
void bongo_cat_mver_pointer_overlay_set(BongoCatMverPointerOverlay *value,
    float x_ratio, float y_ratio, bool left, bool right, bool side);
/*
 * 分两层画, 调用顺序就是遮挡关系, 必须成对地夹住"按键"的绘制:
 *  - before_keys: 手臂与鼠标模式的设备图, 要被按键面板压住;
 *  - after_keys : 非鼠标模式的设备图 (手/手柄), 要压在按键面板之上。
 * 顺序反了会出现"手被键盘挡住"或"鼠标压在键盘上"的穿帮。
 */
void bongo_cat_mver_pointer_overlay_draw_before_keys(
    BongoCatMverPointerOverlay *value);
void bongo_cat_mver_pointer_overlay_draw_after_keys(
    BongoCatMverPointerOverlay *value);

#endif
