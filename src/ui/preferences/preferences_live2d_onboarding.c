/*
 * 缺 Cubism Core 的**启动期引导**(设置页那一行在 preferences_live2d_setup.c)。
 *
 * 为什么单独一个文件: preferences_live2d_setup.c 已经贴着 500 行的策略上限
 * (见 cmake/CheckLines.cmake), 而"打开官方页面 + 一次性的下载教程弹窗"从 URL 策略到按钮语义
 * 自成一块 —— 与 preferences_renderer_row.c 从 preferences_pages.c 拆出来是同一个理由。
 *
 * 三件事在这里:
 *   1. 官方页面的 URL 策略(按界面语言选语言路径, 未实测可达的语言一律回退英文);
 *   2. 那一次性的"下载教程"弹窗(弹窗即教程, **用户点了按钮才打开浏览器**);
 *   3. 触发判据: 已有可用 Core / 缺的是渲染组件 / 用户勾过"不再提示" / 自动引导已做过一次 /
 *      无人值守运行 —— 命中任何一条都不引导。
 */
#include "preferences_live2d_setup.h"

#include "preferences_notice.h"
#include "preferences_state.h"

#include "bongo_cat/i18n.h"
#include "bongo_cat/live2d_core.h"
#include "bongo_cat/log.h"
#include "bongo_cat/preferences.h"
#include "bongo_cat/renderer_module.h"

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

/*
 * "这一次运行的提示机会"。缺 Core 的短提示与启动期的引导弹窗都只能用它一次: 两处各显示一遍
 * 就是同一个问题说两次, 而"不要变成骚扰"是这条流程的硬要求。
 * 抢到的人负责把话说完, 没抢到的直接静默返回。
 */
bool bongo_cat_preferences_live2d_take_prompt_slot(void) {
    static bool used;
    if (used) return false;
    used = true;
    return true;
}

/* 打开官方下载页(按界面语言选语言版本)。失败只记日志: 打开浏览器失败不该影响任何其它功能。 */
void bongo_cat_preferences_live2d_open_download_page(BongoCatApp *app) {
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
            bongo_cat_preferences_live2d_open_download_page(app);
            continue;
        }
        if (pressed == BONGO_CAT_ONBOARDING_IMPORT) {
            SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
                "[live2d] 用户在引导弹窗里选了\"立即导入\", 直接打开文件选择器");
            bongo_cat_preferences_live2d_choose_core_file(app);
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
    if (bongo_cat_preferences_live2d_take_prompt_slot())
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
