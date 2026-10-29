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
/* 把**当前已加载的**模型按现在的图集档位重新加载一遍 (宠物窗口放大之后恢复清晰度)。
 * 与 request_model_switch 的唯一区别是允许目标就是 loaded_model —— 那条"已经是它了,
 * 什么都不用做"的短路在这里必须让路, 否则永远升不上去。旧模型在加载期间照常显示。 */
bool bongo_cat_app_request_model_reload(BongoCatApp *app);
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
/* 影子准备 / 提交 (实现见 model_files.c): 拆开是为了让异步路径能在两者之间插很多帧。
 * force 为真时, "要切换的正好就是当前已加载的模型"这条短路失效 —— 重新加载同一个模型
 * (图集档位升级) 必须真的重走一遍解码与上传。 */
bool bongo_cat_app_begin_model_switch(BongoCatApp *app,
    const BongoCatModelEntry *entry, BongoCatModelSwitchShadow *shadow,
    bool force, BongoCatError *error);
bool bongo_cat_app_commit_model_switch(BongoCatApp *app,
    const BongoCatModelEntry *entry, BongoCatModelSwitchShadow *shadow,
    BongoCatError *error);
/* "要切换的正好就是当前已加载的模型"的收尾 (实现见 model_files.c): 只补一次可能变过的模式
 * 提交, 不重新加载, 也不碰影子。异步队列命中这条路径时**必须**用它 —— 那时影子是空的
 * (从未准备, 或刚被 job_clear 释放), 走 commit_model_switch 会解引用空的 behaviors。 */
bool bongo_cat_app_recommit_loaded_model(BongoCatApp *app,
    const BongoCatModelEntry *entry);

#endif
