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


static const char *tr(BongoCatApp *app, const char *key, const char *fallback) {
    return app && app->i18n ? bongo_cat_i18n_get(app->i18n, key, fallback)
        : fallback;
}

static void show(BongoCatApp *app, const char *message, bool error) {
    if (app && message && message[0])
        bongo_cat_preferences_notice_show(app, message, error);
}



/*
 * 状态文案。**只在"还没有可用的 Core"时被调用**(Core 就位时整行不显示, 见
 * bongo_cat_preferences_live2d_setup_row), 所以这里只剩两种"缺"的说法。
 *
 * 刻意**不**保留"已导入 Cubism Core, Live2D 渲染可用 (csm N)"那条分支: 需求是 Core 就位时
 * 这几行整行不出现, 那条文案因此不会被创建 —— 留一个永远走不到的分支, 正是"看起来查过了、
 * 其实不会发生"的那种代码(这类不可达文案我们刚修过一次)。文案本身仍留在 10 份 locale 里,
 * 将来若要在别处(例如"关于"页)给一句"渲染就绪"的正向确认, 直接拿来用即可。
 */
static void status_text(BongoCatApp *app, char *output, size_t capacity) {
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

void bongo_cat_preferences_live2d_choose_core_file(BongoCatApp *app) {
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
    /*
     * 这几行只在**还没导入 Cubism Core** 时才画。
     *
     * 为什么: Core 就位之后它们全是无用信息 —— 状态行只说一句"已导入"、下载页按钮指向一个
     * 已经不需要的页面、"不再提示"更是失去了对象(没有可提示的东西了)。设置页每多一行不产生
     * 动作的行, 真正要用的行就越难被看见。所以 Core 就位时这三行**整行不出现**(而不是置灰)。
     *
     * 判定用的是既有的运行期事实 `bongo_cat_live2d_core_available()`(别新造一套)。
     * 它是"这一帧"的状态, 刻意**不做**动态热切换: Core 只在程序启动时被采用(导入成功也要
     * 重启一次才参与渲染, 见 live2d_dispatch.h 的 bongo_cat_live2d_native_active 说明), 所以
     * 一次会话里这个判定本来就稳定; 唯一的中间态是"本次运行里刚导入成功"—— 那时 Core 已经
     * 可用, 三行随之消失, 而"要不要现在重启"的弹窗会接手把下一步讲清楚。
     *
     * 缺**渲染组件**是另一回事: 那种情况只给一行纯状态提示, **不**给导入按钮、也不引导去下
     * Core —— 导入 Core 解决不了程序文件缺失(用户该做的是恢复程序文件或重装)。
     */
    const char *title = tr(app, "pages.preference.live2dSetup.title",
        "Live2D Cubism Core");
    if (bongo_cat_live2d_core_available()) return;
    if (!bongo_cat_renderer_module_live2d_available()) {
        bongo_cat_pref_status(context, "live2d-component", title,
            tr(app, "native.live2dComponentMissing",
                "The Live2D renderer component is missing: restore the cores folder "
                "next to the application, or reinstall the application."));
        return;
    }
    char status[512];
    status_text(app, status, sizeof(status));
    const char *button = tr(app, "pages.preference.live2dSetup.choose",
        "Choose SDK zip or Core file...");
    if (bongo_cat_pref_button(context, "live2d-core", title, status, button))
        bongo_cat_preferences_live2d_choose_core_file(app);
    /*
     * 下面两行跟着上面那一行一起出现: 它们的对象都是"缺 Core"这件事 —— 打开下载页是为了去
     * 取 Core, "不再提示"是为了让缺 Core 的自动引导别再烦人。
     *
     * 已经导入好 Core 的用户怎么再导入一次(换版本、换一台机器)? 靠**拖放**: 把官方的 SDK
     * 压缩包或 Core 文件直接拖进设置窗口就会走 Core 导入(见 preferences_import.c 的
     * bongo_cat_preferences_import_path)。刻意**不**再加一个显眼的"更换 Core"按钮 —— 那条路
     * 一年用不到一次, 不值得在设置页上常驻。
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
        bongo_cat_preferences_live2d_open_download_page(app);
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
    if (!bongo_cat_preferences_live2d_take_prompt_slot()) return;
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
