#ifndef BONGO_CAT_PREFERENCES_LIVE2D_SETUP_H
#define BONGO_CAT_PREFERENCES_LIVE2D_SETUP_H

#include "bongo_cat/app.h"
#include "bongo_cat/config.h"
#include "nuklear_config.h"

/*
 * Live2D Cubism Core 的设置入口 (法律合规流程的用户可见部分)。
 *
 * 三件事:
 *   1. 状态行 + "选择压缩包/文件" 按钮: 用户在设置里就能完成"从官网下载 -> 选中 -> 导入";
 *   2. 每个运行周期一次的提示: 缺 Core 或缺渲染组件时各给一条, 且两种原因**分开**说
 *      ("组件缺失"和"没导入 Core"是两件事, 混成一句会让用户白折腾);
 *   3. 拖放路由: 把官方 SDK 压缩包直接拖到设置窗口也能导入。
 *
 * 为什么不做应用内下载: 见 LIVE2D-PLUGIN-PLAN.md §14 —— 代下载法律上更激进, v1 不做。
 * 因此这里只有"打开浏览器"和"读用户自己选的文件"两种动作, 没有任何网络请求。
 */
void bongo_cat_preferences_live2d_setup_row(BongoCatApp *app,
    struct nk_context *context);
/* 每次运行只提示一次(设置窗口打开时调用)。判据是"Live2D 引擎已选中但不可渲染"。 */
void bongo_cat_preferences_live2d_setup_notice(BongoCatApp *app);
/* 拖放入口: 返回真表示这个文件是"Core 相关"(SDK 压缩包或 Core 动态库)并且已被处理,
   调用方不要再把它当模型压缩包去解。判定只看文件名特征, 保守取值 —— 宁可让普通模型包
   继续走模型导入, 也不要把用户的模型 zip 当成 SDK 包去解析。 */
bool bongo_cat_preferences_live2d_setup_drop(BongoCatApp *app, const char *path);

#endif
