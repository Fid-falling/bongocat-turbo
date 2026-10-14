/* 模型切换"影子准备"阶段的内部接口 (仅供 runtime 内部三个文件使用: model_files.c 的提交、
 * model_switch.c 的推进、以及本阶段的实现 model_switch_shadow.c)。
 *
 * BongoCatModelSwitchShadow 本身定义在 bongo_cat/app.h 里 —— 它要内嵌进 BongoCatApp 跨帧
 * 存活, 而 app.h 不可能反过来包含 runtime 内部的头。这里只放函数。
 */
#ifndef BONGO_CAT_MODEL_SWITCH_SHADOW_H
#define BONGO_CAT_MODEL_SWITCH_SHADOW_H

#include "runtime.h"

/* 当前后端是不是 OpenGL (Vulkan 不碰 GL 上下文, 也不读 glGetError)。 */
bool bongo_cat_model_switch_gl_backend(void);
/* 影子准备: 结束快照 -> 捕获行为状态 -> 刷新旧模型封面 -> 行为目录 -> 适配器 profile ->
 * 记录窗口尺寸与上下文。失败时已填 *error 且不留下需要释放的东西。 */
bool bongo_cat_model_switch_prepare(BongoCatApp *app,
    const BongoCatModelEntry *entry, BongoCatModelSwitchShadow *shadow,
    BongoCatError *error);
/* 把主窗口的 GL 上下文切进来 (Vulkan 后端直接返回真, 幂等)。 */
bool bongo_cat_model_switch_activate(BongoCatApp *app,
    BongoCatModelSwitchShadow *shadow, BongoCatError *error);
/* 还原上下文并释放 behaviors (幂等)。提交已经把 behaviors 接手之后调它也安全。 */
void bongo_cat_model_switch_release(BongoCatApp *app,
    BongoCatModelSwitchShadow *shadow);

#endif
