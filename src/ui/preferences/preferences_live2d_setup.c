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
/* "这次到底是谁在渲染"只应该有一处判定: 设置页的状态行问的就是它(见 live2d_dispatch.h)。
   以前这里问的是 bongo_cat_renderer_module_active(), 而那个函数回答的是
   bongo_cat_renderer_module_open() 这条便利路径有没有建过实例 —— 生产路径
   (live2d_dispatch.c 先 load 再直接调 ops->create) 从不走它, 于是状态行永远显示成
   "已导入, 但渲染不可用", 那句"渲染可用"成了不可达文案。 */
#include "live2d_dispatch.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <string.h>

/*
 * 官方下载页。**同一个页面主机, 只换语言路径** —— 主机永远是官方页面主机(守卫允许的那一个),
 * 不是压缩包直链。按界面语言给对应语言版本的页面, 用户就不用在自己看不懂的英文页面上找按钮。
 *
 * 语言路径是**实测**出来的(2026-10, HTTP 状态 + 页面标题; 官网页脚只提供
 * 日本語 / English / 简体中文 / 한국어 / ไทย / Español / Indonesia):
 *   /zh-CHS/sdk/download/native/  -> 200, 标题是中文 ✓
 *   /ja/sdk/download/native/      -> 200, 标题是日文 ✓
 *   /ko/sdk/download/native/      -> 200 ✓
 *   /en/sdk/download/native/      -> 200 ✓
 *   /zh-CHT/...                   -> 404(没有繁体页面) ⇒ 繁体界面**复用简体页**, 比丢到英文页友好
 *   /es/sdk/download/native/      -> 404(没有西班牙语下载页); /es/ 是站点根(200)、
 *                                    /es/sdk/download/ 是**未翻译的日文目录页** —— 都不是
 *                                    能给西语用户用的下载页 ⇒ 西班牙语界面回退 /en/
 *   /th/、/id/                    -> 只有站点根(200), 没有对应下载页; 而且本程序也没有泰语/
 *                                    印尼语界面, 所以没有映射入口(将来加界面语言时, 先实测
 *                                    该语言**下载页**是否可达, 不可达就继续回退 /en/)
 * 结论: 只给 zh-CHS / ja / ko 三个专属路径, **其余(含简体之外的西语等)一律回退 en** ——
 * 宁可用英文页, 也不要把用户送到 404 或一份看不懂的页面。
 */
#define BONGO_CAT_LIVE2D_SDK_PAGE_TEMPLATE \
    "https://www.live2d.com/%s/sdk/download/native/"

/* 界面语言 -> 官方页面的语言路径(未实测可达的语言一律 en, 见上面的实测记录)。 */
static const char *sdk_page_language(BongoCatApp *app) {
    switch (app ? app->settings.app.language : BONGO_CAT_LANG_EN_US) {
    case BONGO_CAT_LANG_ZH_CN:
    case BONGO_CAT_LANG_ZH_HANT: return "zh-CHS"; /* 繁体复用简体页(无繁体页) */
    case BONGO_CAT_LANG_JA_JP: return "ja";
    case BONGO_CAT_LANG_KO_KR: return "ko";
    default: return "en";
    }
}

/* 唯一的 URL 生成点: 引导弹窗、设置页那一行、以及日志都走它, 免得两处各写一份 URL。 */
static bool sdk_page_url(BongoCatApp *app, char *output, size_t capacity) {
    int written = snprintf(output, capacity, BONGO_CAT_LIVE2D_SDK_PAGE_TEMPLATE,
        sdk_page_language(app));
    return written > 0 && (size_t)written < capacity;
}

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

/* 打开官方下载页(按界面语言选语言版本)。失败只记日志: 打开浏览器失败不该影响任何其它功能。 */
static void open_download_page(BongoCatApp *app) {
    char url[192];
    if (!sdk_page_url(app, url, sizeof(url))) {
        SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
            "[live2d] 无法拼出官方下载页地址, 跳过打开浏览器");
        return;
    }
    if (!SDL_OpenURL(url)) {
        SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
            "[live2d] 打开官方 Cubism SDK 下载页失败: %s", SDL_GetError());
        return;
    }
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[live2d] 已在浏览器中打开官方的 Cubism SDK for Native 下载页(界面语言 %s): %s",
        sdk_page_language(app), url);
}

/* 状态描述: 已导入(带版本) / 组件在但缺 Core / 连组件都没有。 */
static void status_text(BongoCatApp *app, char *output, size_t capacity) {
    if (bongo_cat_live2d_core_available()) {
        /* 只有"渲染组件真的活着"才敢说渲染可用 —— 判据必须与审计里
           renderer=cubism-native 那一行同源, 否则会出现"状态行说可用、审计说诊断兜底"
           这种自相矛盾的输出。 */
        const char *text = bongo_cat_live2d_native_active()
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

/*
 * "刚导入成功、还没问用户要不要重启"的待办标记。
 *
 * 为什么需要它(而不是在导入成功的地方直接弹): SDL 的文件对话框回调**可能在另一个线程**上被
 * 调用(SDL_dialog.h 的线程契约), 在那里弹模态框、碰应用状态都是错的。文件对话框那条路本来
 * 也只做"读文件 + 写日志 + 提示"三件事(见文件头)。所以这里只置一个原子标记, 由主线程在
 * 设置窗口渲染时取走并弹窗 —— 那条路每一帧都会跑到, 且导入入口本来就要开着设置窗口。
 */
static SDL_AtomicInt restart_prompt_pending;
static bool restart_prompt_requested;

/*
 * 问一句"要不要现在重启"。
 *
 * 为什么必须问: 导入会立刻换掉磁盘上的 Core, 但**用哪种方式渲染是启动时定下来的**
 * (live2d_dispatch.c 的 component_used 是粘性的, 且启动作实例已经在 app.c 里做完了),
 * 所以这次运行不会当场变身 —— 不重启的话用户导入完看不到猫, 会以为导入失败。
 * 重启复用既有机制(application_restart.c 的 bongo_cat_app_request_restart): 它负责落盘、
 * 还回单实例锁、按原样带过提权状态, 这里不自己拉进程。
 */
static void offer_restart_after_import(BongoCatApp *app) {
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1,
            tr(app, "native.live2dCoreRestartNow", "Restart now")},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0,
            tr(app, "pages.preference.live2dSetup.later", "Later")}
    };
    SDL_MessageBoxData data;
    SDL_zero(data);
    data.flags = SDL_MESSAGEBOX_INFORMATION;
    data.window = app->preferences ? app->preferences->window : NULL;
    data.title = tr(app, "pages.preference.live2dSetup.title",
        "Live2D Cubism Core");
    data.message = tr(app, "native.live2dCoreRestartRequired",
        "Cubism Core was imported, but this run still uses the renderer that was "
        "chosen when it started. Restart once and the model will be drawn with the "
        "newly imported Core.");
    data.numbuttons = (int)SDL_arraysize(buttons);
    data.buttons = buttons;
    int pressed = -1;
    if (!SDL_ShowMessageBox(&data, &pressed)) {
        SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
            "[live2d] Core 已导入, 但\"立即重启\"提示无法显示: %s —— 请手动重启程序",
            SDL_GetError());
        return;
    }
    if (pressed != 1) {
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[live2d] Core 已导入, 用户选择稍后重启(渲染要重启一次才会用它)");
        return;
    }
    if (bongo_cat_app_request_restart(app))
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[live2d] Core 已导入, 用户选择立即重启: 已请求重启以启用新导入的 Core");
    else
        SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
            "[live2d] Core 已导入, 重启请求未被接受(无人值守运行或已有重启在途): 请手动重启");
}

static void import_result(BongoCatApp *app, const char *source) {
    BongoCatLive2DCoreImport report = {0};
    BongoCatError error = {0};
    BongoCatResult result = bongo_cat_live2d_core_import(source, &report, &error);
    char message[512];
    if (result == BONGO_CAT_OK) {
        /* 显示 SHA-256: 用户能拿它和自己下载的文件核对, 也是支持排查时唯一可靠的标识。
           末尾一定要说"重启后生效" —— 导入本身不会让正在运行的这次渲染变身。 */
        snprintf(message, sizeof(message), "%s SHA-256 %s — %s",
            tr(app, "native.live2dCoreImported", "Cubism Core imported successfully."),
            report.sha256,
            tr(app, "native.live2dCoreRestartHint", "restart to apply"));
        show(app, message, false);
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[live2d] 用户导入 Core 成功: %s (来源 %s); 需要重启一次才会用它渲染",
            report.installed, source);
        if (app->automated) {
            SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
                "[live2d] 无人值守运行不弹\"立即重启\": 手动启动时导入完会问一次");
            return;
        }
        SDL_SetAtomicInt(&restart_prompt_pending, 1);
        restart_prompt_requested = true;
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

void bongo_cat_preferences_live2d_restart_prompt_poll(BongoCatApp *app) {
    if (!app || !restart_prompt_requested) return;
    if (!SDL_CompareAndSwapAtomicInt(&restart_prompt_pending, 1, 0)) return;
    restart_prompt_requested = false;
    offer_restart_after_import(app);
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

/*
 * 引导弹窗的按钮。ID 只在本文件内有意义(窗口系统原样回传), 但必须是稳定的正数。
 * 顺序即弹窗上的显示顺序: 主要动作在前, 退出类动作在后。
 */
enum {
    BONGO_CAT_ONBOARDING_DOWNLOAD = 1,
    BONGO_CAT_ONBOARDING_IMPORT = 2,
    BONGO_CAT_ONBOARDING_LATER = 3,
    BONGO_CAT_ONBOARDING_DISMISS = 4
};

/*
 * 缺 Core 的引导弹窗: 弹窗本身就把"怎么做"分步讲完, 需要动作的地方由用户点按钮。
 *
 * 为什么**不自动打开浏览器**: 检测到缺 Core 就 SDL_OpenURL 等于替用户做了一个他没同意的
 * 动作, 而且发生在他还没读完说明之前 —— 页面莫名其妙弹出来, 用户的第一反应是关掉它。
 * 现在打开链接只发生在用户点「打开下载页」之后; 点完**回到弹窗**继续, 因为下载完还得回来
 * 导入, 一次引导要能走完整条路。
 *
 * 按钮语义:
 *   打开下载页 -> 打开官方页面, 然后回到弹窗(还没导入完);
 *   立即导入…  -> 直接开文件选择器, 省掉"回设置窗口再点一次"这一步, 弹窗到此结束;
 *   稍后       -> 关掉弹窗(导入入口与状态行一直留在设置里);
 *   不再提示   -> 关掉并把 live2dCorePromptDismissed 写回配置, 以后不再自动引导。
 */
static void onboarding_dialog(BongoCatApp *app) {
    const char *title = tr(app, "pages.preference.live2dSetup.title",
        "Live2D Cubism Core");
    char message[2048];
    snprintf(message, sizeof(message), "%s", tr(app,
        "pages.preference.live2dSetup.onboarding",
        "The model is not shown because the Live2D Cubism Core runtime library "
        "has not been imported yet. Live2D does not allow us to bundle or download "
        "it, so please get it yourself:\n\n"
        "1. Click \"Open download page\" below to open the official Live2D site.\n"
        "2. Tick the agreement to the software license and privacy policy there.\n"
        "3. Choose \"First download\", then \"Individual\".\n"
        "4. Enter your e-mail address - without it the \"Download the latest "
        "version\" button stays grey and cannot be clicked.\n"
        "5. Click \"Download the latest version\" (not the beta / preview one).\n"
        "6. Back here: click \"Import now...\" and pick the ZIP you just got, or "
        "drag that ZIP straight onto the settings window.\n\n"
        "This library is neither bundled with the application nor downloaded for "
        "you."));
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, BONGO_CAT_ONBOARDING_DOWNLOAD,
            tr(app, "pages.preference.live2dSetup.downloadButton",
                "Open download page")},
        {0, BONGO_CAT_ONBOARDING_IMPORT,
            tr(app, "pages.preference.live2dSetup.importNow", "Import now...")},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, BONGO_CAT_ONBOARDING_LATER,
            tr(app, "pages.preference.live2dSetup.later", "Later")},
        {0, BONGO_CAT_ONBOARDING_DISMISS,
            tr(app, "pages.preference.live2dSetup.dontAsk", "Don't ask again")}
    };
    SDL_MessageBoxData data;
    SDL_zero(data);
    data.flags = SDL_MESSAGEBOX_INFORMATION;
    data.window = app->preferences ? app->preferences->window : NULL;
    data.title = title;
    data.message = message;
    data.numbuttons = (int)SDL_arraysize(buttons);
    data.buttons = buttons;
    for (;;) {
        int pressed = -1;
        if (!SDL_ShowMessageBox(&data, &pressed)) {
            SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE,
                "[live2d] 缺 Core 的引导弹窗无法显示: %s", SDL_GetError());
            return;
        }
        if (pressed == BONGO_CAT_ONBOARDING_DOWNLOAD) {
            /* 只有这里会打开浏览器 —— 用户点了才开。开完回到弹窗, 因为还没导入。 */
            open_download_page(app);
            continue;
        }
        if (pressed == BONGO_CAT_ONBOARDING_IMPORT) {
            SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
                "[live2d] 用户在引导弹窗里选了\"立即导入\", 直接打开文件选择器");
            choose_core_file(app);
            return;
        }
        if (pressed == BONGO_CAT_ONBOARDING_DISMISS) {
            app->settings.app.live2d_core_prompt_dismissed = true;
            app->dirty = true;
            SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
                "[live2d] 用户在引导弹窗里选了\"不再提示\": 写回 live2dCorePromptDismissed");
            return;
        }
        /* 「稍后」与直接关窗都走到这里: 什么都不写, 导入入口照常留在设置里。 */
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[live2d] 用户选择稍后处理(按钮 %d): 设置里的入口照常在", pressed);
        return;
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
        char url[192];
        if (!sdk_page_url(app, url, sizeof(url))) url[0] = '\0';
        SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
            "[live2d] 缺 Cubism Core: 无人值守运行不自动打开设置窗口/弹窗/浏览器; "
            "手动启动时会弹引导(下载页只在用户点按钮后才打开: %s)",
            url[0] ? url : "(地址拼装失败)");
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
    /* 3) 弹一次"下载教程": 内容与动作都在弹窗里, 打开链接必须由用户点按钮触发。 */
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[live2d] 缺 Cubism Core: 显示引导弹窗, 等待用户选择(在此之前不会打开浏览器)");
    onboarding_dialog(app);
    /* 4) 记下"已经引导过一次": 这台机器上不会再自动来一遍(与用户在弹窗里选了什么无关)。 */
    app->settings.app.live2d_core_prompt_shown = true;
    app->dirty = true;
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[live2d] 缺 Cubism Core 的引导已结束(设置窗口 + 弹窗); 下次启动不再自动引导");
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
