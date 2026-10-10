/* 审计场景的辅助入口 (仅 diagnostics 内部使用): input 走完整输入链路 (快捷键 + 输入应用), motion 负责
 * 动作/表情并返回"是否被接受"; parameter_override 判断某类场景施加输入后是否还需额外步进若干帧。
 */
#ifndef BONGO_CAT_LIVE2D_AUDIT_SCENARIO_H
#define BONGO_CAT_LIVE2D_AUDIT_SCENARIO_H

#include "runtime.h"

void bongo_cat_live2d_audit_input(BongoCatApp *app, BongoCatInputKind kind,
    const char *name, float value);
bool bongo_cat_live2d_audit_motion(BongoCatApp *app, const char *scenario);
bool bongo_cat_live2d_audit_parameter_override(const char *scenario);

#endif
