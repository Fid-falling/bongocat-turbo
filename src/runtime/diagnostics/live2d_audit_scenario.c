/* 两个很薄的适配层: 把"合成一条输入事件"和"启动动作/表情"包成审计场景可以直接调用的形式,
 * 让 live2d_audit.c 的场景分发保持无状态。这里不做断言也不写文件, 只转发到模型层。
 */
#include "live2d_audit_scenario.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 名字按事件结构容量截断 (snprintf), 过长的键名不会越界但会被裁剪。 */
void bongo_cat_live2d_audit_input(BongoCatApp *app, BongoCatInputKind kind,
    const char *name, float value) {
    BongoCatInputEvent event = {.kind = kind, .value = value};
    snprintf(event.name, sizeof(event.name), "%s", name);
    bongo_cat_app_shortcuts(app, &event);
    bongo_cat_app_apply_input(app, &event);
}

/* 只认 motion-N 与 expression-N, 其他名字返回 true, 表示"无需额外操作"。 */
bool bongo_cat_live2d_audit_motion(BongoCatApp *app, const char *scenario) {
    if (strcmp(scenario, "motion-0") == 0)
        return bongo_cat_live2d_start_motion(app->live2d, "CAT_motion", 0);
    if (strcmp(scenario, "motion-1") == 0)
        return bongo_cat_live2d_start_motion(app->live2d, "CAT_motion", 1);
    if (strncmp(scenario, "expression-", 11) == 0)
        return bongo_cat_live2d_set_expression(app->live2d,
            atoi(scenario + 11));
    return true;
}

bool bongo_cat_live2d_audit_parameter_override(const char *scenario) {
    return strncmp(scenario, "key-", 4) == 0 ||
        strncmp(scenario, "keys-", 5) == 0 ||
        strncmp(scenario, "gamepad-", 8) == 0 ||
        strcmp(scenario, "mouse-left") == 0 ||
        strcmp(scenario, "mouse-right") == 0;
}
