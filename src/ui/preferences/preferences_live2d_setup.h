#ifndef BONGO_CAT_PREFERENCES_LIVE2D_SETUP_H
#define BONGO_CAT_PREFERENCES_LIVE2D_SETUP_H

#include "bongo_cat/app.h"
#include "bongo_cat/config.h"
#include "nuklear_config.h"

/*
 * Live2D Cubism Core 的设置入口 (法律合规流程的用户可见部分)。
 *
 * 三件事:
 *   1. 状态行 + "选择压缩包/文件" 按钮 + "打开下载页" 按钮 + "不再提示" 勾选: 用户在设置里就能
 *      完成"从官网下载 -> 选中 -> 导入";
 *   2. 每个运行周期一次的提示: 缺 Core 或缺渲染组件时各给一条, 且两种原因**分开**说
 *      ("组件缺失"和"没导入 Core"是两件事, 混成一句会让用户白折腾);
 *   3. 拖放路由: 把官方 SDK 压缩包直接拖到设置窗口也能导入。
 *
 * 为什么不做应用内下载: 见 LIVE2D-PLUGIN-PLAN.md §14 —— 代下载法律上更激进, v1 不做。
 * 因此这里只有"打开浏览器"和"读用户自己选的文件"两种动作, 没有任何网络请求。
 * 这里的 URL 只用于 SDL_OpenURL 把用户送到**官方页面**, 不是压缩包直链 —— 这条区分同时被
 * .github/scripts/check-publish-guards.ps1 当作可执行策略守着。
 */
void bongo_cat_preferences_live2d_setup_row(BongoCatApp *app,
    struct nk_context *context);
/* 每次运行只提示一次(设置窗口打开时调用)。判据是"Live2D 引擎已选中但不可渲染"。 */
void bongo_cat_preferences_live2d_setup_notice(BongoCatApp *app);
/*
 * 启动期的缺 Core 引导: 检测不到可用的 Cubism Core 时, 自动打开设置窗口 + 提示一次 +
 * 把用户送到 Live2D 官方的 Cubism SDK for Native 下载页。满足下面任何一条都不做:
 *   * 已有可用的 Core (绝不打扰已经能用的人);
 *   * 缺的是**渲染组件**(那是"恢复程序文件"的事, 把用户送去下 Core 是错的引导);
 *   * 用户勾过"不再提示", 或已经自动引导过一次(每次安装最多一次, 不做常驻骚扰);
 *   * 无人值守运行(--ci-*): 只记日志 —— 模态弹窗与外开浏览器会打断自动化,
 *     这个约定与 window_menu_actions.c / application_restart.c 里的模态框一致。
 */
void bongo_cat_preferences_live2d_core_onboarding(BongoCatApp *app);
/* 拖放入口: 返回真表示这个文件是"Core 相关"(SDK 压缩包或 Core 动态库)并且已被处理,
   调用方不要再把它当模型压缩包去解。判定只看文件名特征, 保守取值 —— 宁可让普通模型包
   继续走模型导入, 也不要把用户的模型 zip 当成 SDK 包去解析。 */
bool bongo_cat_preferences_live2d_setup_drop(BongoCatApp *app, const char *path);

#endif
