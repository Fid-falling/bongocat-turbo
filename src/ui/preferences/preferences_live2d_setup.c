/*
 * Live2D Cubism Core 的设置入口。见头文件的分工说明。
 *
 * 实现要点与坑:
 *   * SDL_ShowOpenFileDialog 是**异步**的: 回调可能在对话框线程上被调用, 所以回调里
 *     只做"读文件 + 写日志 + 弹提示"这三件线程安全的事, 不碰 nuklear 的状态机。
 *   * 导入是同步做完的(单个 Core 条目 <= 64 MiB, 秒级)。阶段 2 会把"导入 -> 让渲染层
 *     重建模型"接上时再改成后台线程 + 进度; 现在做成线程反而是过早优化, 因为此刻
 *     内置的 Cubism 实现还不能用运行期加载的 Core(它静态链接了导入库)。
 *   * 文案里的 URL 只用于用户自己去浏览器打开: 我们不代下载(§14 的 v1 决策)。
 */
#include "preferences_live2d_setup.h"

#include "preferences_notice.h"
#include "preferences_state.h"
#include "preferences_widgets.h"

#include "bongo_cat/i18n.h"
#include "bongo_cat/live2d_core.h"
#include "bongo_cat/log.h"
#include "bongo_cat/path.h"
#include "bongo_cat/preferences.h"
#include "bongo_cat/renderer_module.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <string.h>

/*
 * Cubism SDK for Native 的**官方下载/说明页**(Live2D 官网)。刻意只写这一个页面地址, 不写压缩包
 * 直链: 直链主机是"程序自己代下载"才需要的东西, 我们用不到; 打开页面则等于让用户自己在官网上
 * 阅读并同意许可 —— 那是唯一合规的取得方式。
 *
 * .github/scripts/check-publish-guards.ps1 有一条可执行策略守着这条区分: 源码里的 live2d 网址
 * 只允许指向官方**页面**主机(本文件下面这一个), 出现别的子域(压缩包直链主机)即失败。所以这个
 * 常量是唯一允许的写法, 别在这里再加"顺手也把直链放上"的地址。
 */
#define BONGO_CAT_LIVE2D_SDK_PAGE "https://www.live2d.com/en/sdk/download/native/"

static const char *tr(BongoCatApp *app, const char *key, const char *fallback) {
    return app && app->i18n ? bongo_cat_i18n_get(app->i18n, key, fallback)
        : fallback;
}

static void show(BongoCatApp *app, const char *message, bool error) {
    if (app && message && message[0])
        bongo_cat_preferences_notice_show(app, message, error);
}

/*
 * "这一次运行的提示机会"。缺 Core 的短提示与启动期的引导弹窗都只能用它一次: 两处各显示一遍
 * 就是同一个问题说两次, 而"不要变成骚扰"是这条流程的硬要求。
 * 抢到的人负责把话说完, 没抢到的直接静默返回。
 */
static bool take_notice_slot(void) {
    static bool used;
    if (used) return false;
    used = true;
    return true;
}

/* 打开官方下载页。失败只记日志: 打开浏览器失败不该影响任何其它功能。 */
static void open_download_page(BongoCatApp *app) {
    if (!SDL_OpenURL(BONGO_CAT_LIVE2D_SDK_PAGE)) {
        SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
            "[live2d] 打开官方 Cubism SDK 下载页失败: %s", SDL_GetError());
        return;
    }
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[live2d] 已在浏览器中打开官方的 Cubism SDK for Native 下载页: %s",
        BONGO_CAT_LIVE2D_SDK_PAGE);
    (void)app;
}

/* 状态描述: 已导入(带版本) / 组件在但缺 Core / 连组件都没有。 */
static void status_text(BongoCatApp *app, char *output, size_t capacity) {
    if (bongo_cat_live2d_core_available()) {
        /* 只有"渲染组件真的活着"才敢说渲染可用。阶段 1 里宿主还没有把渲染切到运行期加载的
           Core 上(编译进去的 Cubism 桥接仍链着导入库), 此时如果说"渲染可用", 用户导入完
           看不到模型就会以为是自己下载错了 —— 所以这里退回"已导入"这句实话。 */
        const char *text = bongo_cat_renderer_module_active()
            ? tr(app, "pages.preference.live2dSetup.installed",
                "Cubism Core imported; Live2D rendering is available")
            : tr(app, "native.live2dCoreImported", "Live2D Cubism Core imported.");
        snprintf(output, capacity, "%s (%s %u)", text, "csm",
            (unsigned)bongo_cat_live2d_core_version());
        return;
    }
    const char *key = bongo_cat_renderer_module_live2d_available()
        ? "pages.preference.live2dSetup.description"
        : "native.live2dComponentMissing";
    const char *fallback = bongo_cat_renderer_module_live2d_available()
        ? "Live2D Cubism Core is not installed yet. Download Cubism SDK for Native "
          "from the official site, then import the ZIP or the Core library here."
        : "The Live2D renderer component is missing from the application's cores "
          "folder; reinstall the application or restore cores/bongo_live2d.";
    snprintf(output, capacity, "%s", tr(app, key, fallback));
}

static void import_result(BongoCatApp *app, const char *source) {
    BongoCatLive2DCoreImport report = {0};
    BongoCatError error = {0};
    BongoCatResult result = bongo_cat_live2d_core_import(source, &report, &error);
    char message[512];
    if (result == BONGO_CAT_OK) {
        /* 显示 SHA-256: 用户能拿它和自己下载的文件核对, 也是支持排查时唯一可靠的标识。 */
        snprintf(message, sizeof(message), "%s SHA-256 %s",
            tr(app, "native.live2dCoreImported", "Cubism Core imported successfully."),
            report.sha256);
        show(app, message, false);
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[live2d] 用户导入 Core 成功: %s (来源 %s)", report.installed, source);
        return;
    }
    snprintf(message, sizeof(message), "%s %s",
        tr(app, "native.live2dCoreImportFailed", "Cubism Core import failed."),
        error.message);
    show(app, message, true);
    SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE, "[live2d] 用户导入 Core 失败(%s): %s",
        source, error.message);
}

/* 对话框回调: 只看第一个文件(这里用的是单选), files 为 NULL 表示用户取消。 */
static void dialog_callback(void *userdata, const char *const *files,
    int filter) {
    (void)filter;
    BongoCatApp *app = userdata;
    if (!app || !files || !files[0] || !files[0][0]) return;
    import_result(app, files[0]);
}

static void choose_core_file(BongoCatApp *app) {
    if (!app) return;
    /* 过滤器按平台给出真实扩展名: 用户从官网下到的是 SDK 压缩包, 也可能只留了动态库。 */
#if defined(_WIN32)
    const char *pattern = "dll;zip";
    const char *title = "Live2DCubismCore.dll / Cubism SDK zip";
#elif defined(__APPLE__)
    const char *pattern = "dylib;zip";
    const char *title = "libLive2DCubismCore.dylib / Cubism SDK zip";
#else
    const char *pattern = "so;zip";
    const char *title = "libLive2DCubismCore.so / Cubism SDK zip";
#endif
    SDL_DialogFileFilter filters[1] = {{title, pattern}};
    SDL_ShowOpenFileDialog(dialog_callback, app, app->window, filters, 1,
        SDL_GetUserFolder(SDL_FOLDER_DOWNLOADS), false);
}

void bongo_cat_preferences_live2d_setup_row(BongoCatApp *app,
    struct nk_context *context) {
    if (!app || !context) return;
    char status[512];
    status_text(app, status, sizeof(status));
    const char *title = tr(app, "pages.preference.live2dSetup.title",
        "Live2D Cubism Core");
    const char *button = tr(app, "pages.preference.live2dSetup.choose",
        "Choose SDK zip or Core file...");
    if (bongo_cat_pref_button(context, "live2d-core", title, status, button))
        choose_core_file(app);
    /*
     * 下面两行**始终**在: 已经导入好 Core 的用户同样需要"打开下载页"这个入口(升级 SDK、
     * 换一台机器), 而一个会自己消失的控件会让人以为设置项丢了。状态行与导入按钮更是一直保留
     * —— 这条流程里唯一会"安静下来"的只有自动引导本身。
     */
    const char *download_title = tr(app, "pages.preference.live2dSetup.download",
        "Cubism SDK for Native");
    const char *download_hint = tr(app,
        "pages.preference.live2dSetup.downloadHint",
        "Opens the official Live2D download page in your browser. This runtime "
        "library is neither bundled nor downloaded for you.");
    const char *download_button = tr(app,
        "pages.preference.live2dSetup.downloadButton", "Open download page");
    if (bongo_cat_pref_button(context, "live2d-core-download", download_title,
            download_hint, download_button))
        open_download_page(app);
    const char *silence_title = tr(app, "pages.preference.live2dSetup.silence",
        "Don't prompt again");
    const char *silence_hint = tr(app,
        "pages.preference.live2dSetup.silenceHint",
        "While Cubism Core is missing, stop opening this window and showing a "
        "notice automatically at startup.");
    if (bongo_cat_pref_toggle(context, "live2d-core-silence", silence_title,
            silence_hint, &app->settings.app.live2d_core_prompt_dismissed)) {
        app->dirty = true;
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[live2d] 缺 Core 的自动引导已%s (写入 live2dCorePromptDismissed)",
            app->settings.app.live2d_core_prompt_dismissed ? "关闭" : "恢复");
    }
}

void bongo_cat_preferences_live2d_setup_notice(BongoCatApp *app) {
    if (!app) return;
    if (bongo_cat_live2d_core_available()) return;
    /* 用户说过"不再提示"就真的不再提示 —— 这句话必须比"每次运行提示一次"更优先。 */
    if (app->settings.app.live2d_core_prompt_dismissed) return;
    if (!take_notice_slot()) return;
    /* 两种缺失分开提示: "组件缺失"和"要导入 Core"是两个动作, 合成一句用户会卡在错的步骤上。 */
    if (bongo_cat_renderer_module_live2d_available()) {
        show(app, tr(app, "native.live2dCoreMissing",
            "Live2D Cubism Core not found: import the Core library or the official "
            "SDK zip in the settings window."), false);
    } else {
        show(app, tr(app, "native.live2dComponentMissing",
            "The Live2D renderer component is missing: restore the cores folder "
            "next to the application, or reinstall the application."), false);
    }
}

void bongo_cat_preferences_live2d_core_onboarding(BongoCatApp *app) {
    if (!app || !app->preferences) return;
    /* 已经有能用的 Core: 绝不打扰。 */
    if (bongo_cat_live2d_core_available()) return;
    /*
     * 缺的是**渲染组件**时走"缺组件"那条路(状态行与提示都会这么说), 但**不**做这里的引导:
     * 用户该做的是恢复程序文件, 把他送去下 Core 只会让他白下几百 MB 还解决不了问题。
     */
    if (!bongo_cat_renderer_module_live2d_available()) {
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[live2d] 缺的是渲染组件而不是 Core: 不做下载引导(缺组件要走恢复程序文件那条路)");
        return;
    }
    if (app->settings.app.live2d_core_prompt_dismissed) {
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[live2d] 用户勾过\"不再提示\": 不自动打开设置窗口, 也不引导下载");
        return;
    }
    if (app->settings.app.live2d_core_prompt_shown) {
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[live2d] 缺 Core 的自动引导已经做过一次: 不再重复(设置里的入口照常在)");
        return;
    }
    /*
     * 无人值守运行(--ci-* 或冒烟)只记日志: 模态弹窗会把脚本挂死, 外开浏览器则是自动化里
     * 不该有的副作用。这条约定与 window_menu_actions.c / application_restart.c 里
     * "无人值守运行一律只记日志"一致。
     */
    if (app->automated) {
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[live2d] 缺 Cubism Core: 无人值守运行不自动打开设置窗口/弹窗/浏览器; "
            "手动启动时会引导到 %s", BONGO_CAT_LIVE2D_SDK_PAGE);
        return;
    }
    /* 1) 先把提示条塞进去, 再开窗口 —— 这样设置窗口的第一帧就带着它(模态弹窗会挡住后续
          帧, 后塞的话用户根本看不到这条提示)。 */
    if (take_notice_slot())
        show(app, tr(app, "native.live2dCoreMissing",
            "Live2D Cubism Core not found: import the Core library or the official "
            "SDK zip in the settings window."), false);
    /* 2) 自动打开设置窗口: 导入入口就在那一行。 */
    bongo_cat_preferences_show(app->preferences);
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[live2d] 缺 Cubism Core: 已自动打开设置窗口(导入入口在 Live2D 那一行)");
    /* 3) 顺手把用户送到官方页面(只打开页面, 不代下载)。
       刻意排在模态弹窗**之前**: 模态框会一直阻塞到这里被点掉, 排在它后面的话浏览器要等用户
       先关掉弹窗才会打开, 上下文就断了(用户点完"确定"什么也没发生, 过一会儿才跳)。 */
    open_download_page(app);
    /* 4) 弹一次告知: 为什么模型不显示、去哪里下载、导入入口在哪。 */
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION,
        tr(app, "pages.preference.live2dSetup.title", "Live2D Cubism Core"),
        tr(app, "pages.preference.live2dSetup.onboarding",
            "The model is not shown because the Live2D Cubism Core runtime "
            "library is not installed yet. Download Cubism SDK for Native from the "
            "official Live2D site and agree to its license, then import the ZIP or "
            "the Core library in the settings window that just opened. This library "
            "is neither bundled nor downloaded for you."),
        app->preferences->window);
    /* 5) 记下"已经引导过一次": 这台机器上不会再自动来一遍。 */
    app->settings.app.live2d_core_prompt_shown = true;
    app->dirty = true;
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[live2d] 缺 Cubism Core 的引导已完成(设置窗口 + 官方下载页 + 弹窗); "
        "下次启动不再自动引导");
}

/* 判定"这个拖进来的文件是不是 Core 相关"。
   只看文件名: 官方压缩包叫 CubismSdkForNative-*.zip, 动态库叫 Live2DCubismCore.dll/
   libLive2DCubismCore.so。刻意不按内容嗅探 —— 那会在拖入模型包时多读一遍磁盘, 而模型导入
   自己也要读。 */
static bool looks_like_core(const char *path) {
    const char *name = bongo_cat_path_name(path);
    if (!name) return false;
    if (strstr(name, "Live2DCubismCore")) return true;
    return strstr(name, "CubismSdkForNative") != NULL;
}

bool bongo_cat_preferences_live2d_setup_drop(BongoCatApp *app,
    const char *path) {
    if (!app || !path || !path[0] || !looks_like_core(path)) return false;
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[live2d] 拖放被识别为 Cubism Core 来源, 走 Core 导入: %s", path);
    import_result(app, path);
    return true;
}
