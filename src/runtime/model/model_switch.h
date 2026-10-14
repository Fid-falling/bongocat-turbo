/* "换桌宠"这条路径的队列与逐帧推进 (实现见 model_switch.c)。
 *
 * 存在的理由: 切换模型要 1~5 秒, 而这段时间主循环必须继续出帧、设置窗口必须继续收输入。
 * 于是所有会换模型的入口都改成"先登记请求, 再由主循环按帧预算推进到底"。
 *
 * 用法:
 *   bongo_cat_app_request_model_switch(app, id, &error);   // 只登记, 立刻返回
 *   ... 主循环每轮调一次 bongo_cat_app_model_switch_step(app, now, 0);
 * 结果通过 bongo_cat_preferences_model_switch_finished / _failed 通知设置窗口。
 * 失败时当前模型保持不变 (这是 model_files.c 里"提交之前不改状态"那条不变量的直接结果)。
 *
 * 队列状态内嵌在 BongoCatApp.model_switch 里 (定义见 bongo_cat/app.h)。
 */
#ifndef BONGO_CAT_MODEL_SWITCH_H
#define BONGO_CAT_MODEL_SWITCH_H

#include "runtime.h"
#include "model_switch_shadow.h"

/* 登记一次切换。已有切换在跑时会把旧的顶掉 (用户连点多个模型 => 只应用最后一次)。 */
bool bongo_cat_app_request_model_switch(BongoCatApp *app, const char *id,
    BongoCatError *error);
/* 推进一次 (主循环每轮调一次)。budget_ns 为 0 时用默认预算。
 * 返回 true = 本帧动过状态 (调用方据此重画); false = 没有正在进行的切换。 */
bool bongo_cat_app_model_switch_step(BongoCatApp *app, uint64_t now,
    uint64_t budget_ns);
/* 正在切换吗? 以及切到哪个 id (没有时返回空串)。 */
bool bongo_cat_app_model_switch_active(const BongoCatApp *app);
const char *bongo_cat_app_model_switch_target(const BongoCatApp *app);
/* 最近一次失败的原因 (只在失败之后有意义)。 */
void bongo_cat_app_model_switch_failure(const BongoCatApp *app,
    BongoCatError *error);
/* 分步加载的进度回调 (注册给 Live2D 加载会话, 实现见 model_files.c)。 */
void bongo_cat_app_model_load_progress(void *userdata, float progress);
/* 影子准备 / 提交 (实现见 model_files.c): 拆开是为了让异步路径能在两者之间插很多帧。 */
bool bongo_cat_app_begin_model_switch(BongoCatApp *app,
    const BongoCatModelEntry *entry, BongoCatModelSwitchShadow *shadow,
    BongoCatError *error);
bool bongo_cat_app_commit_model_switch(BongoCatApp *app,
    const BongoCatModelEntry *entry, BongoCatModelSwitchShadow *shadow,
    BongoCatError *error);

#endif
