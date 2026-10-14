/*
 * "改完设置自动重启" 的调度点 (渲染后端切换、便携模式开关都走这里)。
 *
 * 为什么不在点击的那一帧直接重启:
 *   1. 新设置是 config_store 延迟 300ms 落盘的。立刻重启的话, 新进程读到的是**旧**
 *      配置 —— 重启了等于没重启。所以真正的重启发生在主循环末尾, 紧跟在
 *      bongo_cat_config_store_flush 之后;
 *   2. 重启前必须先把单实例互斥体还回去。不还的话新进程会被判定成"已有实例在运行",
 *      它只会把旧进程唤到前台然后自己退出, 用户看到的是"点了没反应"。
 *
 * 失败回退: 新进程没起来时把互斥体重新抢回来、清掉请求、弹一条"请手动重启", 本进程
 * 继续照常运行。绝不能停在"旧进程已经关了、新进程又没起来"的空档上。
 *
 * 无防循环标记 (方案 B): 系统里**不存在**"启动时自动改设置"的路径 —— 所有会改写配置
 * 并请求重启的入口 (切渲染后端、启停便携模式、提权重启) 都只能由用户主动操作触发,
 * 重启之后必须用户再点一次才会再次触发, 因此不会陷入"重启 -> 又改设置 -> 再重启"的
 * 无限循环。早期为防这种循环而设的 BONGO_CAT_RESTARTED 环境变量已随本方案彻底移除。
 *
 * 这不变量靠命名与注释强制, 不是运行时守卫: 未来任何新增的"自动改设置并重启"来源
 * (初始化、命令行、周期任务) 都必须先回来逐字读这段 —— 它一旦出现, 这里就必须重新
 * 引入防循环, 否则就是无限的自我重启。真到那天, 首选方案是在会自我触发的**唯一**那
 * 个来源处加幂等/一次性守卫, 而不是给全局重启请求加闸。
 */
#include "runtime.h"
#include "portable_mode.h"

#include "bongo_cat/i18n.h"
#include "bongo_cat/log.h"
#include "bongo_cat/platform.h"
#include "bongo_cat/preferences.h"
#include "preferences_notice.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

/* 自动化运行里默认不重启 (冒烟/CI 拉起一个新进程会让脚本挂住), 需要真实验证时用这个
   环境变量显式放行。 */
#define BONGO_CAT_TEST_ALLOW_RESTART_ENV "BONGO_CAT_TEST_ALLOW_RESTART"
#define BONGO_CAT_RESTART_ARGUMENT_CAP 1400

static const char *tr(const BongoCatApp *app, const char *key,
    const char *fallback) {
    return bongo_cat_i18n_get(app->i18n, key, fallback);
}

/*
 * 只有无人值守运行 (--ci-* / 冒烟) 需要拦自动重启: 那种运行里拉起一个新进程会让脚本
 * 挂住, 而且配置照写、重启留给手动是安全的那条路。交互运行永远放行 —— 见文件头的
 * "无防循环标记"一段: 重启只由用户主动操作触发, 不存在需要防的自我循环。
 * BONGO_CAT_TEST_ALLOW_RESTART=1 供 CI 真实验证"落盘 -> 重启 -> 新实例用上新设置"
 * 的链路时显式放行。
 */
static bool restart_blocked(const BongoCatApp *app) {
    if (!app->smoke && !app->automated) return false;
    return SDL_getenv(BONGO_CAT_TEST_ALLOW_RESTART_ENV) == NULL;
}

/* 重启时带哪些参数: 只带"存储位置"这一类。
   - --storage-root=: 用户/CI 显式给的那个 (app->storage_root), 或者当前生效的便携
     目录。便携目录本来就能从标记文件推导出来, 这里显式写上是让新进程的日志一眼能看出
     "这次重启进的是哪个根"; 关掉便携模式之后标记已经删掉, 于是不会再带上旧的便携路径
     (否则用户永远退不回默认目录);
   - --cache-root=: 用户显式给的缓存目录必须带上, 否则重启后缓存换地方;
   - 其它一律不带: --renderer= 会压过刚写进配置的新后端 (正是这次重启要生效的东西),
     --autostart / --ci-* 会让新进程进入后台或无人值守语义。 */
static void restart_arguments(const BongoCatApp *app, char *output,
    size_t capacity) {
    output[0] = '\0';
    const char *storage = app->storage_root;
    char portable[BONGO_CAT_PATH_CAP];
    if (!storage[0] && bongo_cat_portable_mode_active() &&
        bongo_cat_portable_mode_root(portable, sizeof(portable)))
        storage = portable;
    size_t used = 0;
    if (storage[0]) {
        int written = snprintf(output, capacity, "--storage-root=%s", storage);
        if (written < 0 || (size_t)written >= capacity) output[0] = '\0';
        else used = (size_t)written;
    }
    if (app->cache_root_override[0] && used < capacity) {
        int written = snprintf(output + used, capacity - used,
            "%s--cache-root=%s", used ? " " : "", app->cache_root_override);
        if (written < 0 || (size_t)written >= capacity - used)
            output[used] = '\0';
    }
}

/*
 * 重启失败时的提示。设置窗口开着就弹浮动提示 (它在窗口里, 看得见); 关着就只能在托盘
 * 那一侧弹一个模态框 —— 否则用户点了托盘菜单什么都没发生, 日志里才有答案。
 * 无人值守运行 (--ci-* 参数或冒烟) 一律只记日志: 模态框会把脚本挂死。
 */
static void report_manual_restart(BongoCatApp *app, const BongoCatError *error) {
    const char *message = tr(app,
        "composables.useAppMenu.labels.rendererRestartFailed",
        "BongoCat could not restart automatically. Please restart it manually.");
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
        "[runtime] 自动重启失败 (%s); 请手动重启 BongoCat 以应用设置",
        error && error->message[0] ? error->message : "unknown error");
    bongo_cat_preferences_notice_show(app, message, true);
    bool window_visible = app->preferences &&
        bongo_cat_preferences_visible(app->preferences);
    if (window_visible || app->smoke || app->automated ||
        SDL_getenv("BONGO_CAT_TEST_NO_DIALOG")) return;
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, BONGO_CAT_NAME, message,
        app->window);
}

/*
 * 切换渲染后端不再问"现在重启还是稍后", 直接自动重启: 用户点分段控件即代表同意立即
 * 重启。request_restart 内部只会在无人值守运行下拒绝 (见 restart_blocked), 不能自动
 * 重启时由调用方只给提示。
 */
bool bongo_cat_app_request_restart(BongoCatApp *app) {
    return bongo_cat_app_request_restart_elevated(app, false);
}

bool bongo_cat_app_request_restart_elevated(BongoCatApp *app,
    bool elevated) {
    if (!app) return false;
    if (elevated && !app->restart_elevated && app->restart_requested) {
        /* 普通重启已经排上了, 但用户随后又要求提权重启: 升级为提权重启。 */
        app->restart_elevated = true;
        return true;
    }
    if (app->restart_requested) return true;
    /*
     * 只有无人值守运行会被拦 (见 restart_blocked): 交互运行永远放行 —— 重启只由用户
     * 主动操作触发, 无防循环标记需要绕开 (见文件头)。
     */
    if (restart_blocked(app)) {
        /* 无人值守运行 (--ci-* / 冒烟) 下不自动重启, 只能手动, 由调用方提示。 */
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[runtime] 无人值守运行 (%s): 不自动重启, 设置已保存, 请手动重启后生效",
            app->smoke ? "冒烟" : "自动化");
        return false;
    }
    app->restart_requested = true;
    app->restart_elevated = elevated;
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[runtime] 已安排自动重启%s: 新设置先落盘, 主循环末尾重启本程序",
        elevated ? "(提权)" : "");
    return true;
}

/* 主循环末尾调用 (config_store 更新之后): 真正的重启动作。 */
void bongo_cat_app_update_restart(BongoCatApp *app) {
    if (!app || !app->restart_requested) return;
    /*
     * 请求位在成功路径上**不清零**: 它同时是"本次退出是为了换新实例"的标记, 进程入口
     * (app_entry.c) 据此把关闭阶段写成 shutdown:restart, 新进程读到时就不会把上一次
     * 误报成异常退出 (见 startup.c 的判断)。主循环紧接着就会因为 running=false 结束,
     * 不会重复执行到这里。
     */
    /* 新进程是从磁盘读设置的, 这里必须先把待写的设置落下去。 */
    bongo_cat_config_store_flush(app);
    char arguments[BONGO_CAT_RESTART_ARGUMENT_CAP];
    restart_arguments(app, arguments, sizeof(arguments));
    /* 新进程启动会把日志清空, 先把这一份留成 BongoCat.previous.log: 否则"为什么重启"
       的证据会被新实例当场抹掉, 用户回传的日志只剩重启之后的半截。 */
    bongo_cat_runtime_log_preserve();
    /* 先把单实例锁还回去, 新进程才拿得到它。 */
    bongo_cat_platform_single_instance_end();
    BongoCatError error = {0};
    BongoCatResult result = app->restart_elevated
        ? bongo_cat_platform_restart_application_elevated(arguments, &error)
        : bongo_cat_platform_restart_application(arguments, &error);
    if (result == BONGO_CAT_OK) {
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[runtime] 自动重启已发起 (参数: %s), 本进程退出",
            arguments[0] ? arguments : "无");
        app->running = false;
        return;
    }
    /* 失败: 请求位清掉 (让用户之后还能再试), 把锁抢回来并保持原样继续运行。 */
    app->restart_requested = false;
    app->restart_elevated = false;
    if (!bongo_cat_platform_single_instance_begin())
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[runtime] 自动重启失败后未能重新取得单实例锁");
    report_manual_restart(app, &error);
}