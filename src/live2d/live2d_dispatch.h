#ifndef BONGO_CAT_LIVE2D_DISPATCH_H
#define BONGO_CAT_LIVE2D_DISPATCH_H

#include "bongo_cat/common.h"

#include <stdbool.h>

/*
 * 分发层的内部接口(实现见 live2d_dispatch.c)。
 *
 * 这两个查询回答的是"这次运行到底是谁在渲染":
 *   * `bongo_cat_live2d_native_active()` —— 组件已装载并接管渲染(macOS/Linux 与 Windows
 *     一致)。诊断输出(renderer=cubism-native)与设置页状态都必须问它, 而**不能**只看编译期
 *     宏: 组件缺失/被停用/ABI 不匹配时宏仍然是"这个构建支持 Live2D", 但实际跑的是诊断实现,
 *     那时写 cubism-native 就是撒谎(而且会让 CI 的视觉验收失去意义)。
 *   * `bongo_cat_live2d_component_path()` —— 已装载组件的路径, 供日志与支持排查。
 */
bool bongo_cat_live2d_native_active(void);
const char *bongo_cat_live2d_component_path(void);
/*
 * 首次需要真实例之前装载组件(幂等)。
 *
 * Vulkan 侧要在**窗口创建**时就问"有没有组件"(决定这次走不走 VK 渲染器), 而那时还没建任何
 * 实例 —— 所以这一步必须能被外部提前触发; 否则懒装载会让宿主误判成"这次没有 VK 渲染器",
 * 于是静默回退 GL(踩过: 表现就是强制 VK 时模型加载失败)。
 */
bool bongo_cat_live2d_ensure_component(void);

#endif
