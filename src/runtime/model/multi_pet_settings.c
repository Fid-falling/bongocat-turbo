/* 多桌宠的文件请求协议 (父、子两个方向共用一套约定): 子进程把 pass-through.request 与
 * primary.request 写进自己的状态目录, 父进程每帧取走并删除 —— 请求都是一次性的,
 * 先删后判, 内容坏掉也只报错不重放。反方向 (父进程的实时可见性与穿透) 走 control.txt,
 * 见 multi_pet_process.c。两侧读同一批文件, 所以路径都从 primary_state_root 加模型哈希拼出来。
 */
#include "runtime.h"
#include "bongo_cat/file.h"
#include "bongo_cat/path.h"
#include "bongo_cat/preferences.h"

#include <stdio.h>
#include <string.h>

#define PASS_THROUGH_REQUEST_FILE "pass-through.request"
#define PRIMARY_REQUEST_FILE "primary.request"

/* 拼 <primary_state_root>/pets/<hash>/<请求文件>: 所有调用方都传 primary_state_root,
 * 请求文件固定放在"模型自己"的状态目录里。
 */
static bool request_path(char *target, size_t capacity, const char *root,
    const char *model_id, const char *request_file) {
    char directory[BONGO_CAT_PATH_CAP];
    return bongo_cat_multi_pet_state_directory(directory, sizeof(directory),
            root, model_id) &&
        bongo_cat_path_join(target, capacity, directory, request_file);
}

/* 启动新子进程之前清掉历史穿透请求, 否则它第一帧就会把旧的穿透状态盖回主窗口。 */
void bongo_cat_multi_pet_clear_pass_through_request(BongoCatApp *app,
    const char *model_id) {
    char path[BONGO_CAT_PATH_CAP];
    if (app && request_path(path, sizeof(path), app->primary_state_root,
            model_id, PASS_THROUGH_REQUEST_FILE) &&
        bongo_cat_path_is_file(path) &&
        !bongo_cat_file_remove(path))
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[runtime] Stale multi-pet pass-through request could not be "
            "removed: model=%s path=%s", model_id, path);
}

/* 同上, 清的是 preferences / exit 一类主进程请求; 删不掉只告警, 不阻塞启动。 */
void bongo_cat_multi_pet_clear_primary_request(BongoCatApp *app,
    const char *model_id) {
    char path[BONGO_CAT_PATH_CAP];
    if (app && request_path(path, sizeof(path), app->primary_state_root,
            model_id, PRIMARY_REQUEST_FILE) && bongo_cat_path_is_file(path) &&
        !bongo_cat_file_remove(path))
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[runtime] Stale multi-pet primary request could not be removed: "
            "model=%s path=%s", model_id, path);
}

/* 写请求的公共实现: 先写 .tmp 再原子替换。只允许副进程调用 (app->secondary_pet 必须为真),
 * 因为请求文件写的是"自己"的状态目录, 主进程没有这个目录。
 */
static bool write_primary_request(BongoCatApp *app, const char *action) {
    char path[BONGO_CAT_PATH_CAP], temporary[BONGO_CAT_PATH_CAP];
    int length;
    if (!app || !app->secondary_pet ||
        !request_path(path, sizeof(path), app->primary_state_root,
            app->secondary_model_id, PRIMARY_REQUEST_FILE) ||
        (length = snprintf(temporary, sizeof(temporary), "%s.tmp", path)) < 0 ||
        (size_t)length >= sizeof(temporary)) return false;
    FILE *file = bongo_cat_file_open(temporary, "wb");
    bool written = file && fprintf(file, "%s\n", action) > 0;
    if (file && fclose(file) != 0) written = false;
    if (written && bongo_cat_file_replace(temporary, path, true)) return true;
    bongo_cat_file_remove(temporary);
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
        "[runtime] Multi-pet primary request failed: model=%s action=%s "
        "path=%s", app->secondary_model_id, action, path);
    return false;
}

/* 副桌宠请求主实例打开设置窗口或整体退出; 主进程在下一次 primary_requests_update 时消费。 */
bool bongo_cat_multi_pet_request_preferences(BongoCatApp *app) {
    return write_primary_request(app, "preferences");
}

bool bongo_cat_multi_pet_request_exit(BongoCatApp *app) {
    return write_primary_request(app, "exit");
}

/* 副桌宠请求切换穿透 (用户在副桌宠的托盘菜单里操作): 写进自己目录的请求文件,
 * 由父进程统一应用到主窗口, 避免两个进程同时改窗口的点击穿透。
 */
bool bongo_cat_multi_pet_request_pass_through(BongoCatApp *app,
    bool enabled) {
    char path[BONGO_CAT_PATH_CAP], temporary[BONGO_CAT_PATH_CAP];
    int length;
    if (!app || !app->secondary_pet ||
        !bongo_cat_path_join(path, sizeof(path), app->state_root,
            PASS_THROUGH_REQUEST_FILE) ||
        (length = snprintf(temporary, sizeof(temporary), "%s.tmp", path)) < 0 ||
        (size_t)length >= sizeof(temporary)) return false;
    FILE *file = bongo_cat_file_open(temporary, "wb");
    bool written = file && fprintf(file, "%d\n", enabled) > 0;
    if (file && fclose(file) != 0) written = false;
    if (!written || !bongo_cat_file_replace(temporary, path, true)) {
        bongo_cat_file_remove(temporary);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[runtime] Multi-pet pass-through request failed: model=%s "
            "path=%s", app->secondary_model_id, path);
        return false;
    }
    return true;
}

/* 父进程取走穿透请求: 文件存在就先删再判内容 —— 内容非法也不重放, 否则会一直失败下去。
 * 只接受 "0"/"1" 加尾部空白; 返回 false 表示这次没有可用请求 (含文件不存在)。
 */
static bool take_request(BongoCatApp *app, const char *model_id,
    bool *enabled) {
    char path[BONGO_CAT_PATH_CAP], content[8] = {0};
    if (!request_path(path, sizeof(path), app->primary_state_root, model_id,
            PASS_THROUGH_REQUEST_FILE) ||
        !bongo_cat_path_is_file(path)) return false;
    FILE *file = bongo_cat_file_open(path, "rb");
    if (!file) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[runtime] Multi-pet pass-through request could not be read: "
            "model=%s path=%s", model_id, path);
        return false;
    }
    size_t length = file ? fread(content, 1, sizeof(content) - 1, file) : 0;
    if (file) fclose(file);
    if (!bongo_cat_file_remove(path)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[runtime] Multi-pet pass-through request could not be consumed: "
            "model=%s path=%s", model_id, path);
        return false;
    }
    content[length] = '\0';
    bool valid = content[0] == '0' || content[0] == '1';
    for (size_t i = 1; valid && content[i]; ++i)
        valid = content[i] == ' ' || content[i] == '\t' ||
            content[i] == '\r' || content[i] == '\n';
    if (!valid) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[runtime] Multi-pet pass-through request invalid: model=%s "
            "path=%s content=%s", model_id, path, content);
        return false;
    }
    *enabled = content[0] == '1';
    return true;
}

typedef enum PrimaryRequest {
    PRIMARY_REQUEST_NONE,
    PRIMARY_REQUEST_PREFERENCES,
    PRIMARY_REQUEST_EXIT
} PrimaryRequest;

/* 取走并解析 primary.request: 去掉尾部空白后整词匹配, 不认识的内容报错并按"没有请求"处理
 * (文件这时已经被删掉了)。
 */
static PrimaryRequest take_primary_request(BongoCatApp *app,
    const char *model_id) {
    char path[BONGO_CAT_PATH_CAP], content[32] = {0};
    if (!request_path(path, sizeof(path), app->primary_state_root, model_id,
            PRIMARY_REQUEST_FILE) || !bongo_cat_path_is_file(path))
        return PRIMARY_REQUEST_NONE;
    FILE *file = bongo_cat_file_open(path, "rb");
    if (!file) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[runtime] Multi-pet primary request could not be read: "
            "model=%s path=%s", model_id, path);
        return PRIMARY_REQUEST_NONE;
    }
    size_t length = fread(content, 1, sizeof(content) - 1, file);
    fclose(file);
    if (!bongo_cat_file_remove(path)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[runtime] Multi-pet primary request could not be consumed: "
            "model=%s path=%s", model_id, path);
        return PRIMARY_REQUEST_NONE;
    }
    while (length && (content[length - 1] == ' ' ||
        content[length - 1] == '\t' || content[length - 1] == '\r' ||
        content[length - 1] == '\n')) length--;
    content[length] = '\0';
    if (!strcmp(content, "preferences")) return PRIMARY_REQUEST_PREFERENCES;
    if (!strcmp(content, "exit")) return PRIMARY_REQUEST_EXIT;
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
        "[runtime] Multi-pet primary request invalid: model=%s path=%s",
        model_id, path);
    return PRIMARY_REQUEST_NONE;
}

/* 父进程每帧处理所有副桌宠的 primary 请求: exit 立即停掉整个应用并放弃其余请求;
 * preferences 只置标志, 循环结束后统一打开 —— 同一个窗口不能被开两次。子进程直接返回。
 */
void bongo_cat_multi_pet_primary_requests_update(BongoCatApp *app) {
    if (!app || app->secondary_pet) return;
    bool show_preferences = false;
    for (size_t i = 0; i < app->session.additional_model_count; ++i) {
        PrimaryRequest request = take_primary_request(app,
            app->session.additional_model_ids[i]);
        if (request == PRIMARY_REQUEST_EXIT) {
            app->running = false;
            return;
        }
        if (request == PRIMARY_REQUEST_PREFERENCES) show_preferences = true;
    }
    if (show_preferences) bongo_cat_preferences_show(app->preferences);
}

/* 把各副桌宠的穿透请求应用到主窗口: 同一帧多个请求时后者覆盖前者, 最后统一置命中脏位、
 * 同步穿透并让设置界面失效。
 */
void bongo_cat_multi_pet_pass_through_requests_update(BongoCatApp *app) {
    if (!app || app->secondary_pet) return;
    bool changed = false;
    for (size_t i = 0; i < app->session.additional_model_count; ++i) {
        bool enabled;
        const char *model_id = app->session.additional_model_ids[i];
        if (!take_request(app, model_id, &enabled)) continue;
        app->settings.window.pass_through = enabled;
        changed = true;
    }
    if (!changed) return;
    bongo_cat_window_mark_hit_dirty(app);
    bongo_cat_window_sync_click_through(app);
    bongo_cat_preferences_invalidate(app->preferences);
}
