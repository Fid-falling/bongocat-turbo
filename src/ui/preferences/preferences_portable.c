/*
 * 设置 -> 应用 区的"便携模式"开关: 把六棵存储树换成程序目录下的 BongoCatData\。
 *
 * 这一行本身不做任何判断: "开没开"与"怎么搬"都在 runtime/lifecycle/portable_mode.c
 * 里, 这里只负责交互与反馈。判定复用同一个入口很重要 —— 自己拼一遍 <exe>\BongoCatData
 * 就是第二套解析, 迟早和运行时对不上。
 *
 * 为什么开关要分两帧才真的动手: 复制设置与模型是同步的 (几百 MB 时能卡住一秒以上),
 * 而用户点完开关的第一帧本来可以用来画"正在迁移"。所以状态机是
 *   1 → 2 → 3 → 执行
 * 前两步只画"进行中"并把这一帧端出去, 第三步才真的复制。省掉它, 用户看到的就是界面
 * 卡住然后突然跳到结果 —— 与"迁移过程要有可见反馈"的要求正好相反。
 *
 * 失败语义: 迁移函数自己保证"要么搬完、要么目标回滚", 这里只负责把错误原样报给用户。
 * 关掉便携模式时如果合并失败, 便携模式会保持开启 (标记不删), 提示里说的也是"没有改动"。
 */
#include "preferences_internal.h"
#include "preferences_notice.h"
#include "preferences_state.h"
#include "preferences_widgets.h"
#include "bongo_cat/i18n.h"
#include "bongo_cat/log.h"
#include "portable_mode.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

/* 状态机的三个档位: |值| == 3 时才真的执行迁移, 前两档只把"进行中"画出去。 */
#define PORTABLE_STEP_LIMIT 3

static const char *tr(BongoCatApp *app, const char *key, const char *fallback) {
    return bongo_cat_i18n_get(app->i18n, key, fallback);
}

/*
 * 真的搬数据。成功了再请求重启 —— 存储根是启动时解析的, 运行中换不了; 而且新进程必须
 * 从新的位置读设置, 所以"迁移完就重启"是唯一正确的顺序。
 */
static void portable_apply(BongoCatPreferences *value, bool enable) {
    BongoCatApp *app = value->app;
    BongoCatError error = {0};
    BongoCatResult result = enable
        ? bongo_cat_portable_mode_enable(app, &error)
        : bongo_cat_portable_mode_disable(app, &error);
    if (result != BONGO_CAT_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[portable] %s失败: %s", enable ? "开启" : "关闭",
            error.message[0] ? error.message : "unknown error");
        /* 报错文案优先用底层那句 (它带着具体路径/原因), 拿不到才退回通用提示。 */
        bongo_cat_preferences_notice_show(app, error.message[0] ? error.message :
            tr(app, "pages.preference.general.hints.portableFailed",
                "Could not switch the portable mode; nothing was changed"), true);
        value->render_dirty = true;
        return;
    }
    bool restarting = bongo_cat_app_request_restart(app);
    bongo_cat_preferences_notice_show(app, restarting
        ? tr(app, "pages.preference.general.hints.portableRestarting",
            "Data migrated. Restarting BongoCat to apply the new location...")
        : tr(app, "pages.preference.general.hints.portableManualRestart",
            "Data migrated. Restart BongoCat manually to use the new location."),
        false);
    if (!restarting)
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[portable] 自动重启未执行, 请手动重启以使用新的存储位置");
    value->render_dirty = true;
}

/*
 * 测试钩子: BONGO_CAT_TEST_PORTABLE_TOGGLE=on|off 会在这一帧模拟"用户点了这个开关"。
 * 走的是和真实点击完全相同的那条路 (状态机 + 迁移 + 请求重启), 所以能覆盖
 * "开启 -> 迁移 -> 重启 -> 新实例读写便携目录"的整条链路。
 */
static void portable_test_hook(BongoCatPreferences *value) {
    static bool hook_used;
    const char *hook = SDL_getenv("BONGO_CAT_TEST_PORTABLE_TOGGLE");
    if (hook_used || !hook || !hook[0]) return;
    /* 等窗口真的显示出来再动手: 真实用户只能点看得见的窗口, 而且"正在迁移/失败"这类
       反馈只有窗口可见时才有意义 —— 窗口还藏着的时候触发, 提示会在没人看见的时候过期。 */
    if (!value->visible) return;
    if (strcmp(hook, "on") != 0 && strcmp(hook, "off") != 0) return;
    hook_used = true;
    if (value->portable_migration_pending) return;
    value->portable_migration_pending = strcmp(hook, "on") == 0 ? 1 : -1;
    value->render_dirty = true;
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[portable] 测试钩子: 模拟把便携模式%s", strcmp(hook, "on") == 0
            ? "打开" : "关闭");
}

void bongo_cat_preferences_portable_draw(BongoCatPreferences *value,
    struct nk_context *context) {
    if (!value || !value->app) return;
    BongoCatApp *app = value->app;
    portable_test_hook(value);
    int pending = value->portable_migration_pending;
    bool busy = pending != 0;
    bool enabled = bongo_cat_portable_mode_active();
    const char *title = tr(app, "pages.preference.general.labels.portableMode",
        "Portable mode");
    const char *detail = busy
        ? tr(app, "pages.preference.general.hints.portableMigrating",
            "Migrating settings and models, please wait...")
        : tr(app, "pages.preference.general.hints.portableMode",
            "Keep every file in a BongoCatData folder next to the executable; "
            "copy the program together with that folder to migrate. Settings "
            "and models are copied, caches are rebuilt on demand.");
    bongo_cat_pref_row_icon(context, BONGO_CAT_PREF_ICON_RENDERER);
    bool target = enabled;
    /* 迁移进行中也要照常画这一行 (它占着表单的一列, 少画一次整页布局会错位),
       只是这一次点击不生效。 */
    if (bongo_cat_pref_toggle(context, "portable-mode", title, detail, &target) &&
        !busy) {
        /* 只置状态机的第一档: 真正的复制在后面几帧发生, 好让"进行中"先露个面。 */
        value->portable_migration_pending = target ? 1 : -1;
        bongo_cat_preferences_notice_show(app, tr(app,
            "pages.preference.general.hints.portableMigrating",
            "Migrating settings and models, please wait..."), false);
        value->render_dirty = true;
    }
    /*
     * 状态机。这里用的是本帧开始时的 pending: 用户刚点完的那一帧只把"进行中"排上,
     * 复制留到后面帧 —— 见文件头注释。
     */
    if (pending > 0 && pending < PORTABLE_STEP_LIMIT) {
        value->portable_migration_pending = pending + 1;
        value->render_dirty = true;
    } else if (pending < 0 && -pending < PORTABLE_STEP_LIMIT) {
        value->portable_migration_pending = pending - 1;
        value->render_dirty = true;
    } else if (pending == PORTABLE_STEP_LIMIT ||
        pending == -PORTABLE_STEP_LIMIT) {
        value->portable_migration_pending = 0;
        portable_apply(value, pending > 0);
    }
}
