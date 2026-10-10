/* 多桌宠的父进程侧: 除主实例外, 每个被勾选的模型都由一个独立子进程显示, 两者用状态目录里的
 * 文件做 IPC —— 父进程写 control.txt (可见性 + 时间戳 + 穿透) 当心跳, 子进程写
 * remove.request / pass-through.request / primary.request 提请求。
 * 子进程读不到足够新的心跳就自杀, 所以父进程必须每 2 s 续写一次 (见 multi_pet_process.c);
 * 目录名是模型 id 的 FNV-1a 哈希, 父子两侧都靠它对齐, 算法一改两边就对不上。
 * 本文件同时含子进程侧的读取逻辑 (bongo_cat_multi_pet_update 的另一半分支)。
 */
#include "runtime.h"
#include "bongo_cat/file.h"
#include "bongo_cat/path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CONTROL_FILE "control.txt"
#define REMOVE_REQUEST_FILE "remove.request"
#define CONTROL_CHECK_NS 100000000ull
#define CONTROL_STALE_SECONDS 10
#define CONTROL_RETRY_GRACE_NS 5000000000ull
#define SETTINGS_CHECK_NS 2000000000ull

/* 读 control.txt 的三种结果: OK 采纳本帧; RETRY 是暂时性问题 (文件还没写出来、格式不对),
 * 由调用方在宽限期内继续重试; END 表示父进程已经不在了或状态非法, 子进程应当退出。
 */
typedef enum ControlReadResult {
    CONTROL_READ_OK, CONTROL_READ_RETRY, CONTROL_READ_END
} ControlReadResult;

typedef struct ControlState {
    bool visible;
    bool pass_through;
} ControlState;

/* FNV-1a 64 位哈希, 只为把模型 id 变成稳定的目录名: 父子两个进程必须算出同一个值,
 * 所以算法与下面的 %016llx 格式都不能单方面改。
 */
static uint64_t model_hash(const char *model_id) {
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char *p = (const unsigned char *)model_id; p && *p; ++p) {
        hash ^= *p;
        hash *= 1099511628211ull;
    }
    return hash;
}

/* 生成 <root>/pets/<hash>。子进程的 state_root 就是它, 父进程从 primary_state_root
 * 拼出同一路径, 用来找子进程写下的请求文件。
 */
bool bongo_cat_multi_pet_state_directory(char *target, size_t capacity,
    const char *root, const char *model_id) {
    char pets[BONGO_CAT_PATH_CAP], name[24];
    snprintf(name, sizeof(name), "%016llx",
        (unsigned long long)model_hash(model_id));
    return root && model_id && model_id[0] &&
        bongo_cat_path_join(pets, sizeof(pets), root, "pets") &&
        bongo_cat_path_join(target, capacity, pets, name);
}

static bool primary_remove_request_path(char *target, size_t capacity,
    const BongoCatApp *app, const char *model_id) {
    char directory[BONGO_CAT_PATH_CAP];
    return app && bongo_cat_multi_pet_state_directory(directory,
        sizeof(directory), app->primary_state_root, model_id) &&
        bongo_cat_path_join(target, capacity, directory, REMOVE_REQUEST_FILE);
}

/* 父进程取走"移除这个副桌宠"请求: 文件真的删掉才算消费成功, 删不掉就报错返回 false,
 * 免得留下它让下次启动重复移除。
 */
bool bongo_cat_multi_pet_take_remove_request(BongoCatApp *app,
    const char *model_id) {
    char path[BONGO_CAT_PATH_CAP];
    if (!primary_remove_request_path(path, sizeof(path), app, model_id) ||
        !bongo_cat_path_is_file(path)) return false;
    if (bongo_cat_file_remove(path)) return true;
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
        "[runtime] Multi-pet remove request could not be consumed: "
        "model=%s path=%s", model_id, path);
    return false;
}

/* 启动新子进程之前清理上一次留下的移除请求, 否则新进程一启动就会被摘掉。 */
void bongo_cat_multi_pet_clear_remove_request(BongoCatApp *app,
    const char *model_id) {
    char path[BONGO_CAT_PATH_CAP];
    if (primary_remove_request_path(path, sizeof(path), app, model_id) &&
        bongo_cat_path_is_file(path)) bongo_cat_file_remove(path);
}

/* 当前进程是否由 --secondary-pet=<非空 id> 启动; 前缀自带等号, 空值不算命中。 */
bool bongo_cat_multi_pet_secondary_argument(int argc, char **argv) {
    static const char prefix[] = "--secondary-pet=";
    for (int i = 1; i < argc; ++i)
        if (!strncmp(argv[i], prefix, sizeof(prefix) - 1) &&
            argv[i][sizeof(prefix) - 1]) return true;
    return false;
}

/* 子进程侧: 往自己的 state_root 写 remove.request, 请求父进程把它从会话里摘掉。
 * 先写 .tmp 再原子替换, 父进程不会读到半截文件。
 */
bool bongo_cat_multi_pet_request_remove(BongoCatApp *app) {
    char path[BONGO_CAT_PATH_CAP], temporary[BONGO_CAT_PATH_CAP];
    int length;
    if (!app || !app->secondary_pet ||
        !bongo_cat_path_join(path, sizeof(path), app->state_root,
            REMOVE_REQUEST_FILE) ||
        (length = snprintf(temporary, sizeof(temporary), "%s.tmp", path)) < 0 ||
        (size_t)length >= sizeof(temporary)) return false;
    FILE *file = bongo_cat_file_open(temporary, "wb");
    bool written = file && fprintf(file, "%s\n", app->secondary_model_id) > 0;
    if (file && fclose(file) != 0) written = false;
    if (!written || !bongo_cat_file_replace(temporary, path, true)) {
        bongo_cat_file_remove(temporary);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[runtime] Multi-pet remove request failed: model=%s path=%s",
            app->secondary_model_id, path);
        return false;
    }
    return true;
}

/* 读父进程的心跳, 格式为 "<visible|hidden> <unix 秒> <pass_through>" 三个字段。
 * 时间戳用来判断父进程是否还活着 —— 过期或来自未来都当成父进程已死, 返回 END 让子进程退出;
 * 文件暂时不存在或格式坏掉则是 RETRY, 由调用方的宽限期兜底。
 */
static ControlReadResult read_control(BongoCatApp *app,
    ControlState *control, const char **retry_reason) {
    char path[BONGO_CAT_PATH_CAP], content[64] = {0};
    if (!bongo_cat_path_join(path, sizeof(path), app->state_root,
            CONTROL_FILE)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[runtime] Multi-pet child control path is too long: model=%s",
            app->secondary_model_id);
        return CONTROL_READ_END;
    }
    FILE *file = bongo_cat_file_open(path, "rb");
    if (!file) {
        *retry_reason = "missing";
        return CONTROL_READ_RETRY;
    }
    size_t length = file ? fread(content, 1, sizeof(content) - 1, file) : 0;
    if (file) fclose(file);
    content[length] = '\0';
    char state[8] = {0}, extra = 0;
    long long heartbeat = 0;
    int pass_through = app->settings.window.pass_through;
    int fields = sscanf(content, "%7s %lld %d %c", state, &heartbeat,
        &pass_through, &extra);
    bool valid = fields == 3 &&
        (pass_through == 0 || pass_through == 1);
    time_t now = time(NULL), then = (time_t)heartbeat;
    if (!valid) {
        *retry_reason = "invalid-format";
        return CONTROL_READ_RETRY;
    }
    if (now < then || now - then > CONTROL_STALE_SECONDS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[runtime] Multi-pet child control expired: model=%s "
            "reason=stale age=%lld path=%s", app->secondary_model_id,
            (long long)(now - then), path);
        return CONTROL_READ_END;
    }
    control->visible = !strcmp(state, "visible");
    if (!control->visible && strcmp(state, "hidden")) {
        *retry_reason = "invalid-state";
        return CONTROL_READ_RETRY;
    }
    control->pass_through = pass_through != 0;
    return CONTROL_READ_OK;
}

/* 子进程跟随主实例的设置: 每 2 s 重读共享的 settings.json。pass_through 是例外,
 * 它以 control.txt 为准 (那才是父进程的实时状态), 否则会被滞后的设置文件盖回去。
 * 窗口段变化要重新应用置顶、命中测试与穿透同步。
 */
static void reload_secondary_settings(BongoCatApp *app) {
    BongoCatSettings settings = app->settings;
    BongoCatError error = {0};
    BongoCatResult loaded = bongo_cat_settings_load(
        app->settings_path, &settings, &error);
    if (loaded != BONGO_CAT_OK) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[runtime] Multi-pet child settings sync failed: model=%s "
            "result=%d error=%s", app->secondary_model_id, loaded,
            error.message[0] ? error.message : "unknown error");
        return;
    }
    if (app->secondary_control_known)
        settings.window.pass_through = app->secondary_control_pass_through;
    bool model_changed = memcmp(&settings.model, &app->settings.model,
        sizeof(settings.model)) != 0;
    bool window_changed = memcmp(&settings.window, &app->settings.window,
        sizeof(settings.window)) != 0;
    app->settings = settings;
    if (model_changed) app->dirty = true;
    if (window_changed) {
        app->dirty = true;
        bongo_cat_platform_set_always_on_top(&app->platform,
            app->settings.window.always_on_top);
        bongo_cat_window_mark_hit_dirty(app);
        bongo_cat_window_sync_click_through(app);
    }
}

/* 子进程的循环钩子: 每 100 ms 检查一次心跳, 每 2 s 重读设置。
 * 第一次失败只记下起点, 连续失败超过 5 s 宽限才退出 —— 主进程被强杀时子进程不会变成孤儿。
 */
static void update_secondary(BongoCatApp *app, uint64_t now) {
    if (!app->secondary_control_check_ns ||
        now >= app->secondary_control_check_ns) {
        ControlState control = {0};
        const char *retry_reason = "unknown";
        ControlReadResult result = read_control(app, &control, &retry_reason);
        if (result == CONTROL_READ_END) app->running = false;
        else if (result == CONTROL_READ_RETRY) {
            if (!app->secondary_control_failure_ns)
                app->secondary_control_failure_ns = now;
            else if (now - app->secondary_control_failure_ns >=
                CONTROL_RETRY_GRACE_NS) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[runtime] Multi-pet child control unavailable: model=%s "
                    "reason=%s", app->secondary_model_id, retry_reason);
                app->running = false;
            }
        } else {
            bool visibility_changed = !app->secondary_control_known ||
                control.visible != app->session.window.visible;
            bool pass_changed = !app->secondary_control_known ||
                control.pass_through != app->settings.window.pass_through;
            app->secondary_control_failure_ns = 0;
            app->secondary_control_known = true;
            app->secondary_control_visible = control.visible;
            app->secondary_control_pass_through = control.pass_through;
            if (pass_changed) {
                app->settings.window.pass_through = control.pass_through;
                bongo_cat_window_mark_hit_dirty(app);
                bongo_cat_window_sync_click_through(app);
            }
            if (visibility_changed)
                bongo_cat_window_set_visible(app, control.visible);
        }
        app->secondary_control_check_ns = now + CONTROL_CHECK_NS;
    }
    if (!app->secondary_settings_check_ns ||
        now >= app->secondary_settings_check_ns) {
        reload_secondary_settings(app);
        app->secondary_settings_check_ns = now + SETTINGS_CHECK_NS;
    }
}

/* 唯一的调度入口: now 由主循环传入 (SDL_GetTicksNS), 保证同一帧内所有判定用同一时间基准。
 * 子进程分支只做心跳与设置同步, 父进程分支才跑完整的多桌宠状态机。
 */
void bongo_cat_multi_pet_update(BongoCatApp *app, uint64_t now) {
    if (!app) return;
    if (app->secondary_pet) update_secondary(app, now);
    else bongo_cat_multi_pet_primary_update(app, now);
}
