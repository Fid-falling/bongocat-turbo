/* 指针审计的输出契约: 数组下标固定按 tl/tr/bl/br 排列, maximum_step 是"相邻帧参数跳变"而非总位移,
 * has_mouse 表示模型是否提供 ParamMouseX/Y; live2d_audit.c 直接读这些字段, 改序会同时影响那边断言。
 */
#ifndef BONGO_CAT_LIVE2D_POINTER_AUDIT_H
#define BONGO_CAT_LIVE2D_POINTER_AUDIT_H

#include "runtime.h"

typedef struct BongoCatPointerAudit {
    bool ran;
    bool has_mouse;
    float angle_x[4];
    float angle_y[4];
    float mouse_x[4];
    float mouse_y[4];
    float maximum_step;
} BongoCatPointerAudit;

extern BongoCatPointerAudit bongo_cat_pointer_audit;

bool bongo_cat_live2d_pointer_audit_run(BongoCatApp *app, bool mirror);
bool bongo_cat_live2d_pointer_reverse_audit_run(BongoCatApp *app);

#endif
