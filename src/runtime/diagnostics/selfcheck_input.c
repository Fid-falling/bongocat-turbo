/*
 * 自检第 7 组: 输入。
 *
 * 只读状态, **不做深度测试**: 真实的按键注入会污染用户正在用的输入队列, 还要求窗口
 * 有焦点; 自检只要能回答"接收器线程还在跑吗"就够了 —— 它不在跑时按键不会有任何反应,
 * 而那正是用户会误报成"桌宠坏了"的情形。
 */
#include "selfcheck_internal.h"

#include "bongo_cat/app.h"
#include "bongo_cat/platform.h"

#include <stdio.h>

#ifdef _WIN32
#include "windows_input.h"
#endif

void bongo_cat_selfcheck_input(BongoCatSelfCheckBuilder *builder,
    struct BongoCatApp *app) {
    bongo_cat_selfcheck_section(builder, "输入");
    if (!app) {
        bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_FAIL, "输入接收器",
            "应用状态未就绪，无法确认输入接收器");
        return;
    }
#ifdef _WIN32
    if (bongo_cat_platform_input_receiver_active(&app->platform))
        bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_PASS, "输入接收器",
            "Raw Input 接收线程正在运行（键盘/鼠标输入会送到模型）");
    else
        bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_FAIL, "输入接收器",
            "Raw Input 接收线程未运行：键盘/鼠标输入不会生效（常见原因是"
            "接收器启动失败或已被其它实例占用，详见日志 [input] 行）");
#else
    bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_WARN, "输入接收器",
        "当前平台未提供接收器状态查询（仅 Windows 构建提供该检查）");
#endif
}
