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

#endif
