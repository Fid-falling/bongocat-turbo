/*
 * 命令行解析, 分三类:
 *   - 用户/系统参数: --autostart, --storage-root=, --cache-root=, --nearby-root=,
 *     --secondary-pet= / --secondary-position= (多宠子进程由主进程拉起);
 *   - 逃生参数: --renderer=auto|vulkan|opengl (简写 --vulkan / --opengl) 与 --help。
 *     后端的覆盖值优先级高于 settings.json, 因为"设置窗口画不出来"恰恰会发生在
 *     用户最需要切后端的时候 —— 那时 GUI 已经不是可用的入口了;
 *   - --ci-* 冒烟参数: 只影响这一次运行, 用来无人值守地驱动设置窗口、导入模型、跑
 *     自测; 它们不写回配置 (config_store 在 smoke 下不落盘)。
 * 不认识的参数静默忽略 —— Windows 会往命令行里塞一些系统注入的参数。
 */
#include "runtime.h"
#include "bongo_cat/i18n.h"
#include "bongo_cat/render.h"

#include <SDL3/SDL.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include "windows_package.h"
#endif

/* --ci-model-switch= 的默认时序 (见 lifecycle/smoke_model_switch.c):
   第一帧之后 2.5s 才开始登记 (让启动自己先收尾), 两次登记之间 120ms —— 比"同一帧连发"
   更接近真实连点: 上一次切换已经跑出十几个半成品步骤, 下一个请求必须走 supersede。 */
#define SMOKE_SWITCH_DELAY_DEFAULT_NS 2500000000ull
#define SMOKE_SWITCH_GAP_DEFAULT_NS 120000000ull

static bool store_argument(char *target, size_t capacity, const char *value,
    const char *name, BongoCatError *error) {
    int length = snprintf(target, capacity, "%s", value ? value : "");
    if (length >= 0 && (size_t)length < capacity) return true;
    bongo_cat_error_set(error, BONGO_CAT_ERROR_ARGUMENT,
        "%s path is too long", name);
    return false;
}

/*
 * --ci-model-switch=id1,id2,... 的解析 (驱动器见 lifecycle/smoke_model_switch.c)。
 * 逗号分隔而不是重复给同一个参数: 顺序是这条自测的全部意义 ("最后一个是哪一个"), 写在
 * 一行里才看得清。id 超长或整串写不下都直接失败 —— 被静默截断的模型 id 会让断言以
 * "模型不存在"的形式误报, 比启动失败难查得多。给多了只 WARN 并截断到上限。
 */
static bool parse_switch_list(BongoCatApp *app, const char *value,
    BongoCatError *error) {
    const char *cursor = value ? value : "";
    unsigned dropped = 0;
    while (*cursor) {
        const char *comma = strchr(cursor, ',');
        size_t length = comma ? (size_t)(comma - cursor) : strlen(cursor);
        if (length) {
            if (app->smoke_switch.count >= BONGO_CAT_SMOKE_SWITCH_CAP) dropped++;
            else if (length >= BONGO_CAT_ID_CAP) {
                bongo_cat_error_set(error, BONGO_CAT_ERROR_ARGUMENT,
                    "Model id is too long: %.*s", (int)length, cursor);
                return false;
            } else {
                char *target =
                    app->smoke_switch.ids[app->smoke_switch.count++];
                memcpy(target, cursor, length);
                target[length] = '\0';
            }
        }
        if (!comma) break;
        cursor = comma + 1;
    }
    if (dropped)
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "--ci-model-switch keeps at most %d models; ignoring %u more",
            BONGO_CAT_SMOKE_SWITCH_CAP, dropped);
    app->smoke_switch.enabled = app->smoke_switch.count > 0;
    return true;
}

/* --ci-keys=KeyA,KeyS,... 的解析 (注入在 lifecycle/app.c 尾部)。与 parse_switch_list
   同一个逗号格式, 但写的是输入注入清单: 数量给多了只 WARN 并截断到上限。 */
static bool parse_key_list(BongoCatApp *app, const char *value,
    BongoCatError *error) {
    const char *cursor = value ? value : "";
    unsigned dropped = 0;
    while (*cursor) {
        const char *comma = strchr(cursor, ',');
        size_t length = comma ? (size_t)(comma - cursor) : strlen(cursor);
        if (length) {
            if (app->smoke_keys_count >= BONGO_CAT_SMOKE_SWITCH_CAP) dropped++;
            else if (length >= BONGO_CAT_ID_CAP) {
                bongo_cat_error_set(error, BONGO_CAT_ERROR_ARGUMENT,
                    "Key name is too long: %.*s", (int)length, cursor);
                return false;
            } else {
                char *target = app->smoke_keys[app->smoke_keys_count++];
                memcpy(target, cursor, length);
                target[length] = '\0';
            }
        }
        if (!comma) break;
        cursor = comma + 1;
    }
    if (dropped)
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "--ci-keys keeps at most %d keys; ignoring %u more",
            BONGO_CAT_SMOKE_SWITCH_CAP, dropped);
    return true;
}

/* 毫秒参数 (--ci-model-switch-delay-ms= / -gap-ms=): 只接受纯数字, 认不出来的值按默认值
   处理并 WARN —— 自测参数写错时"跑起来用的是默认值"比"启动失败"好查。 */
static uint64_t milliseconds_argument(const char *value, uint64_t fallback) {
    if (!value || !*value) return fallback;
    char *end = NULL;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (!end || *end || parsed > UINT64_MAX / 1000000ull) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Ignoring malformed millisecond argument: %s", value);
        return fallback;
    }
    return (uint64_t)parsed * 1000000ull;
}

/*
 * 用法输出 (--help / -h)。写在这里而不是 app_entry.c: 参数的唯一真相就是本文件,
 * 拆成两个地方迟早会对不上。
 *
 * 刻意用英文: 这段文字在 SDL/日志初始化之前就要打出来, 那时 i18n 表还没加载;
 * 混排中英反而更难读。需要中文说明时看 docs/ 或 README。
 */
void bongo_cat_cli_print_usage(FILE *out) {
    if (!out) out = stdout;
    fprintf(out,
        BONGO_CAT_NAME " " BONGO_CAT_VERSION "\n"
        "\n"
        "Usage: BongoCat [options]\n"
        "\n"
        "Renderer (highest priority: overrides settings.json, this run only):\n"
        "  --renderer=auto|vulkan|opengl  Force the render backend.\n"
        "  --vulkan                       Shorthand for --renderer=vulkan.\n"
        "  --opengl                       Shorthand for --renderer=opengl (escape hatch\n"
        "                                 when the Vulkan UI does not draw correctly).\n"
        "\n"
        "Paths:\n"
        "  --storage-root=DIR   Root for settings/session/models/logs.\n"
        "  --cache-root=DIR     Cache root override.\n"
        "  --nearby-root=DIR    Extra directory to scan for models.\n"
        "\n"
        "Other:\n"
        "  --autostart                  Mark this launch as an autostart launch.\n"
        "  --secondary-pet=MODEL_ID     Run as an additional pet process.\n"
        "  --secondary-position=X,Y     Initial position for the additional pet.\n"
        "  --help, -h                   Show this help and exit.\n"
        "\n"
        "Environment:\n"
        "  BONGO_CAT_VULKAN_VALIDATION=1 Enable the Vulkan validation layer\n"
        "                               (needs the SDK layer installed; messages are\n"
        "                               written to the application log).\n"
        "  BONGO_CAT_PREFERENCES_OPENGL=1 Draw the settings window with OpenGL even when\n"
        "                               the Vulkan backend is active.\n"
        "  BONGO_CAT_PREFS_VSYNC=1      Use FIFO present mode for the settings window.\n");
}

/* 只扫参数、不建 app: --help 必须在单实例锁和存储路径之前就能回答, 否则用户在
   配置坏掉/实例已运行时连用法都看不到。 */
bool bongo_cat_cli_wants_help(int argc, char **argv) {
    if (!argv) return false;
    for (int i = 1; i < argc; ++i) {
        if (!argv[i]) continue;
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0 ||
            strcmp(argv[i], "/?") == 0) return true;
    }
    return false;
}

/* nearby_root 只是"额外多扫一个目录"的来源, 取不到就不设 (不算失败); 真正由
   --nearby-root= 给的路径太长才报错。 */
static bool prepare_nearby_root(BongoCatApp *app, BongoCatError *error) {
#ifdef _WIN32
    /* A packaged full-trust app is commonly launched with System32 as its
       current directory.  That is not a user model directory, and scanning
       it can walk protected Windows management folders (for example
       appmgmt\MACHINE), causing startup to fail or consume the scan budget.
       Nearby model discovery remains available through an explicit
       --nearby-root argument and for unpackaged/portable launches. */
    char *current = NULL;
    const char *root = NULL;
    if (!bongo_cat_windows_is_packaged()) {
        current = SDL_GetCurrentDirectory();
        root = current && current[0] ? current : SDL_GetBasePath();
    }
#else
    const char *root = SDL_GetBasePath();
#endif
    bool stored = !root || store_argument(app->nearby_root,
        sizeof(app->nearby_root), root, "Nearby model", error);
#ifdef _WIN32
    SDL_free(current);
#endif
    return stored;
}

/* 先算 nearby_root (它要用工作目录/程序目录, 跟命令行无关), 再逐条解析: 带路径的参数
   写不下时直接失败 —— 被静默截断的路径比启动失败难查得多。 */
bool bongo_cat_startup_arguments(BongoCatApp *app, int argc, char **argv,
    BongoCatError *error) {
    if (!prepare_nearby_root(app, error)) return false;
    app->smoke_switch.delay_ns = SMOKE_SWITCH_DELAY_DEFAULT_NS;
    app->smoke_switch.gap_ns = SMOKE_SWITCH_GAP_DEFAULT_NS;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        /* --ci-* 一律算无人值守运行: 模态弹窗在那种运行里会把进程挂死, 见 app.h。 */
        if (strncmp(arg, "--ci-", 5) == 0) app->automated = true;
        if (strcmp(arg, "--autostart") == 0) app->autostart_launch = true;
        /*
         * 后端覆盖。三条写法同一个目标, 解析成功就置 app->renderer_override
         * (int, -1 = 未覆盖)。识别不了的 --renderer= 值只 WARN 不失败: 启动失败
         * 会把用户彻底挡在门外, 而"参数没生效"至少还能进主界面再改设置。
         */
        else if (strcmp(arg, "--opengl") == 0 || strcmp(arg, "--gl") == 0)
            app->renderer_override = (int)BONGO_CAT_RENDERER_OPENGL;
        else if (strcmp(arg, "--vulkan") == 0 || strcmp(arg, "--vk") == 0)
            app->renderer_override = (int)BONGO_CAT_RENDERER_VULKAN;
        else if (strncmp(arg, "--renderer=", 11) == 0) {
            BongoCatRenderer parsed;
            if (bongo_cat_renderer_parse(arg + 11, &parsed))
                app->renderer_override = (int)parsed;
            else
                fprintf(stderr, "BongoCat: ignoring unknown --renderer=%s "
                    "(expected auto, vulkan or opengl)\n", arg + 11);
        }
        else if (strcmp(arg, "--ci-smoke") == 0) app->smoke = true;
        else if (strcmp(arg, "--ci-preferences") == 0) app->smoke_preferences = true;
        else if (strcmp(arg, "--ci-preference-shortcut") == 0)
            app->smoke_preference_shortcut = true;
        else if (strcmp(arg, "--ci-preference-model-select") == 0)
            app->smoke_preference_model_select = true;
        else if (strcmp(arg, "--ci-remove-imported") == 0) app->smoke_remove_imported = true;
        else if (strcmp(arg, "--ci-shortcuts") == 0) app->smoke_shortcuts = true;
        else if (strcmp(arg, "--ci-menu") == 0) app->smoke_menu = true;
        else if (strcmp(arg, "--ci-ignore-global-input") == 0) app->smoke_ignore_global_input = true;
        else if (strcmp(arg, "--ci-pass-through") == 0) app->smoke_pass_through = true;
        else if (strcmp(arg, "--ci-context-menu") == 0) app->smoke_context_menu = true;
        else if (strcmp(arg, "--ci-frame-series") == 0) app->smoke_frame_series = true;
        else if (strcmp(arg, "--ci-runtime-flow") == 0) app->smoke_runtime_flow = true;
        else if (strcmp(arg, "--ci-freeze-model") == 0) app->smoke_freeze_model = true;
        else if (strcmp(arg, "--ci-sound-test") == 0) app->smoke_sound_test = true;
        else if (strncmp(arg, "--ci-sound-toggle=", 18) == 0)
            app->smoke_sound_toggle = (unsigned)atoi(arg + 18);
        else if (strncmp(arg, "--ci-preference-page=", 21) == 0) {
            int page = atoi(arg + 21);
            if (page >= 0 && page < 4) app->smoke_preference_page = page;
        } else if (strncmp(arg, "--ci-language=", 14) == 0) {
            BongoCatLanguage language;
            if (bongo_cat_language_parse(arg + 14, &language))
                app->smoke_language = language;
        } else if (strncmp(arg, "--ci-theme=", 11) == 0) {
            const char *name = arg + 11;
            for (int value = 0; value <= BONGO_CAT_THEME_DARK; ++value)
                if (strcmp(name, bongo_cat_theme_name((BongoCatTheme)value)) == 0)
                    app->smoke_theme = value;
        } else if (strncmp(arg, "--ci-exit-ms=", 13) == 0) {
            uint64_t delay = strtoull(arg + 13, NULL, 10);
            app->smoke_deadline_ns = delay > UINT64_MAX / 1000000ull
                ? UINT64_MAX : delay * 1000000ull;
        } else if (strncmp(arg, "--storage-root=", 15) == 0) {
            if (!store_argument(app->storage_root, sizeof(app->storage_root),
                arg + 15, "Storage", error)) return false;
        } else if (strncmp(arg, "--cache-root=", 13) == 0) {
            if (!store_argument(app->cache_root_override,
                sizeof(app->cache_root_override), arg + 13,
                "Cache", error)) return false;
        } else if (strncmp(arg, "--nearby-root=", 14) == 0) {
            if (!store_argument(app->nearby_root, sizeof(app->nearby_root),
                arg + 14, "Nearby model", error)) return false;
        } else if (strncmp(arg, "--secondary-pet=", 16) == 0) {
            if (!store_argument(app->secondary_model_id,
                sizeof(app->secondary_model_id), arg + 16,
                "Secondary model", error)) return false;
            app->secondary_pet = app->secondary_model_id[0] != '\0';
        } else if (strncmp(arg, "--secondary-position=", 21) == 0) {
            char *separator = NULL, *end = NULL;
            long x = strtol(arg + 21, &separator, 10);
            long y = separator && *separator == ',' ?
                strtol(separator + 1, &end, 10) : 0;
            app->secondary_origin_known = separator != arg + 21 && end &&
                end != separator + 1 && !*end && x >= INT_MIN && x <= INT_MAX &&
                y >= INT_MIN && y <= INT_MAX;
            if (app->secondary_origin_known) {
                app->secondary_origin_x = (int)x;
                app->secondary_origin_y = (int)y;
            }
        } else if (strncmp(arg, "--ci-import=", 12) == 0) {
            if (!store_argument(app->smoke_import_path, sizeof(app->smoke_import_path),
                arg + 12, "Import", error)) return false;
        } else if (strncmp(arg, "--ci-model=", 11) == 0) {
            if (!store_argument(app->smoke_model, sizeof(app->smoke_model),
                arg + 11, "Model", error)) return false;
        } else if (strncmp(arg, "--ci-model-switch=", 18) == 0) {
            if (!parse_switch_list(app, arg + 18, error)) return false;
        } else if (strncmp(arg, "--ci-keys=", 10) == 0) {
            if (!parse_key_list(app, arg + 10, error)) return false;
        } else if (strncmp(arg, "--ci-model-switch-delay-ms=", 27) == 0) {
            app->smoke_switch.delay_ns = milliseconds_argument(arg + 27,
                SMOKE_SWITCH_DELAY_DEFAULT_NS);
        } else if (strncmp(arg, "--ci-model-switch-gap-ms=", 25) == 0) {
            app->smoke_switch.gap_ns = milliseconds_argument(arg + 25,
                SMOKE_SWITCH_GAP_DEFAULT_NS);
        } else if (strncmp(arg, "--ci-viewer-trace=", 18) == 0) {
            if (!store_argument(app->smoke_viewer_trace,
                sizeof(app->smoke_viewer_trace), arg + 18,
                "Viewer trace", error)) return false;
        } else if (strncmp(arg, "--ci-live2d-scenario=", 21) == 0 &&
            !store_argument(app->smoke_live2d_scenario,
                sizeof(app->smoke_live2d_scenario), arg + 21,
                "Scenario", error)) return false;
    }
/* 没显式给 --ci-exit-ms= 的冒烟运行统一 1.5s 后自动退出, 免得 CI 挂住。
   --ci-sound-test 也按这个规则: 它是一个"跑完就退"的诊断, 不能要求调用方再给一个
   退出时间, 否则忘写一次就留下一个常驻进程。 */
    if (!app->smoke_deadline_ns &&
        (app->smoke || app->smoke_sound_test))
        app->smoke_deadline_ns = 1500000000ull;
    return true;
}
