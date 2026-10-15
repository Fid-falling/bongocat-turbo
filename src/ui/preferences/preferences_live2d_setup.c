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
#include "preferences_widgets.h"

#include "bongo_cat/i18n.h"
#include "bongo_cat/live2d_core.h"
#include "bongo_cat/log.h"
#include "bongo_cat/path.h"
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
}

void bongo_cat_preferences_live2d_setup_notice(BongoCatApp *app) {
    static bool shown;
    if (!app || shown) return;
    shown = true;
    if (bongo_cat_live2d_core_available()) return;
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
