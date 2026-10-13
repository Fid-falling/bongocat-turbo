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
 * 防重启循环: 环境变量 BONGO_CAT_RESTARTED=1 会跟着新进程走 (见平台层的实现), 带这个
 * 标记的实例不再自动重启, 只提示手动重启。没有这道闸, 任何"启动时就会改设置"的路径
 * 都会变成无限重启。
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

#define BONGO_CAT_RESTARTED_ENV "BONGO_CAT_RESTARTED"
/* 自动化运行里默认不重启 (冒烟/CI 拉起一个新进程会让脚本挂住), 需要真实验证时用这个
   环境变量显式放行。 */
#define BONGO_CAT_TEST_ALLOW_RESTART_ENV "BONGO_CAT_TEST_ALLOW_RESTART"
#define BONGO_CAT_RESTART_ARGUMENT_CAP 1400

static const char *tr(BongoCatApp *app, const char *key, const char *fallback) {
    return bongo_cat_i18n_get(app->i18n, key, fallback);
}

static bool restart_blocked(const BongoCatApp *app) {
    if (SDL_getenv(BONGO_CAT_RESTARTED_ENV)) return true;
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
 * 问一次"现在重启还是稍后手动重启"。返回 true = 用户选了立即重启。
 *
 * 为什么要问: 切换渲染后端必须重启才生效, 而重启会关掉正在跑的窗口 (桌宠也会闪一下)。
 * 用户点了分段控件不代表他同意现在就重启, 所以这一步不能替他决定。
 *
 * 无人值守运行 (--ci-* / 冒烟 / BONGO_CAT_TEST_NO_DIALOG) 一律不弹窗, 直接按"稍后"
 * 处理: 模态框会把脚本挂死, 而"稍后手动重启"本来就是安全的那条路 (配置照写)。
 */
bool bongo_cat_app_confirm_restart(BongoCatApp *app) {
    if (!app) return false;
    if (app->smoke || app->automated || SDL_getenv("BONGO_CAT_TEST_NO_DIALOG")) {
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[runtime] 无人值守运行 (%s): 不弹重启确认框, 按\"稍后手动重启\"处理",
            app->smoke ? "冒烟" : app->automated ? "--ci-* 参数"
                : "BONGO_CAT_TEST_NO_DIALOG");
        return false;
    }
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1,
            tr(app, "composables.useAppMenu.labels.rendererRestartConfirmNow",
                "Restart now")},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0,
            tr(app, "composables.useAppMenu.labels.rendererRestartConfirmLater",
                "I will restart later")},
    };
    SDL_MessageBoxData data = {0};
    data.flags = SDL_MESSAGEBOX_INFORMATION;
    data.window = app->window;
    data.title = BONGO_CAT_NAME;
    data.message = tr(app,
        "composables.useAppMenu.labels.rendererRestartConfirm",
        "Switching the render backend needs a restart of BongoCat to take "
        "effect. Restart now?");
    data.numbuttons = 2;
    data.buttons = buttons;
    int button = -1;
    if (!SDL_ShowMessageBox(&data, &button)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[runtime] 重启确认框打不开 (%s), 按\"稍后手动重启\"处理",
            SDL_GetError());
        return false;
    }
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[runtime] 重启确认框: 用户选择 %s",
        button == 1 ? "立即重启" : "稍后手动重启");
    return button == 1;
}

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
    if (restart_blocked(app)) {
        /* 这里不是错误: 带了防循环标记说明本次运行就是"刚刚被重启出来的", 再重启一次
           只会套娃; 自动化运行同理。两种情况都要求手动重启, 由调用方提示。 */
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[runtime] 本次运行不再自动重启 (%s), 设置已保存, 请手动重启后生效",
            SDL_getenv(BONGO_CAT_RESTARTED_ENV) ? "已经自动重启过一次"
                : "自动化运行");
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