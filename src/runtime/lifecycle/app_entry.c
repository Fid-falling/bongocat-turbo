/*
 * 进程入口。职责很窄, 但退出码与单实例配对是外部可见的契约:
 *   - 初始化失败返回 1, 正常退出返回 app->exit_code (CI / 更新流程会改写它);
 *   - 单实例锁只有主进程参与 (副宠允许并存), 而且每一条 return 路径都必须 begin/end
 *     配对 —— 漏掉一次 end, 用户下次启动就会被误判成"已有实例"。
 *   - 拿不到单实例锁 = 已有实例在跑: begin 内部已经把"打开设置窗口"的唤醒请求发给它
 *     (见 platform.h 的 BongoCatWakeRequest), 本进程随即返回 0 退出。所以第二次双击
 *     图标不是"什么都没发生", 而是"运行中的那个实例弹出设置窗口"。
 *   - update-shutdown 参数是更新器要求"退出去装包"的信号, 直接返回 0, 不建窗口。
 *   - --help 必须排在单实例锁之前: 用户往往是在"已经有一个实例在跑"或"配置坏掉"
 *     的情况下才来查用法, 这两种情况都不该把帮助挡掉。
 */
#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include "windows_diagnostics.h"
#endif

int bongo_cat_app_run(int argc, char **argv) {
    /*
     * 崩溃兜底必须最先装: 用户报的"进程闪一下就消失"发生在启动首帧附近, 而 Windows 的
     * 默认处理多半是静默退出。装上之后, 未处理的异常码与出错模块名会先落进日志
     * (见 windows_crash.c), 再交回系统默认处理。
     */
#ifdef _WIN32
    bongo_cat_windows_install_crash_handler();
#endif
    if (bongo_cat_cli_wants_help(argc, argv)) {
        bongo_cat_cli_print_usage(stdout);
        return 0;
    }
    if (bongo_cat_platform_update_shutdown_argument(argc, argv)) return 0;
    /* 自动更新的隐藏助手模式 (--apply-update): 等旧进程退出 -> 替换 exe -> 重启。
       必须排在单实例锁之前 —— 助手进程就是被替换的那个 exe, 去抢锁只会自己挡自己;
       也绝不能走到 SDL/窗口初始化: 那时用户已经关掉程序了, 助手只做文件操作。 */
    int apply_exit_code = 0;
    if (bongo_cat_platform_update_apply_argument(argc, argv, &apply_exit_code))
        return apply_exit_code;
    bool secondary = bongo_cat_multi_pet_secondary_argument(argc, argv);
    if (!secondary && !bongo_cat_platform_single_instance_begin()) return 0;
/* 还没有 app 可传, 所以 startup_failure(NULL) 只打日志; 但单实例锁仍要归还。 */
    BongoCatApp *app = calloc(1, sizeof(*app));
    if (!app) {
        BongoCatError memory = {0};
        bongo_cat_error_set(&memory, BONGO_CAT_ERROR_MEMORY,
            "Cannot allocate application state");
        bongo_cat_startup_failure(NULL, &memory);
        if (!secondary) bongo_cat_platform_single_instance_end();
        return 1;
    }
    BongoCatError error = {0};
    if (!bongo_cat_app_initialize(app, argc, argv, &error)) {
        bongo_cat_startup_failure(app, &error);
        if (app->smoke) bongo_cat_startup_ci_failure(app, &error);
        bongo_cat_app_shutdown(app, "shutdown:startup-failure", 1);
        free(app);
        if (!secondary) bongo_cat_platform_single_instance_end();
        return 1;
    }
    bongo_cat_app_loop(app);
/* exit_code 在主循环里可能被 CI / 更新流程改过, 关闭前取一次, 同时用于 shutdown
   与进程返回值, 保证"日志里记的"和"系统看到的"一致。 */
    int exit_code = app->exit_code;
/* restart_requested 在 update_restart 里被改写: 真的发起了重启时它保持置位, 于是这里
   把关闭阶段写成 shutdown:restart —— 新进程据此知道上一次是"退出去换新实例", 不会误报
   成异常退出 (见 startup.c)。自动重启时单实例锁已经还回去了, 下面的 end 是幂等的。 */
    bongo_cat_app_shutdown(app, app->restart_requested
        ? "shutdown:restart" : "shutdown:normal", exit_code);
    free(app);
    if (!secondary) bongo_cat_platform_single_instance_end();
    return exit_code;
}
