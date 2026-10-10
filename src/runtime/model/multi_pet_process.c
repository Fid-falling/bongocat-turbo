/* 多桌宠的父进程实现: 为每个勾选的模型拉起一个独立子进程 (同一个可执行文件加
 * --secondary-pet=<id>), 并用状态目录里的 control.txt 把主窗口的可见性与穿透状态同步过去。
 * 时序契约: 先 write_control 再 spawn —— 子进程启动后立刻读心跳, 读不到会重试并在宽限期后退出;
 * 之后每 CONTROL_HEARTBEAT_NS 续一次。停止时先删 control.txt, 超时则强杀。
 * 启动失败累计 3 次会把模型从会话里摘掉并提示用户, 避免每帧重启一个坏进程。
 */
#include "runtime.h"
#include "preferences_notice.h"
#include "bongo_cat/file.h"
#include "bongo_cat/i18n.h"
#include "bongo_cat/path.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CONTROL_FILE "control.txt"
#define CONTROL_HEARTBEAT_NS 2000000000ull
#define CHILD_RETRY_NS 2000000000ull
#define CHILD_STOP_NS 1000000000ull
/* 一个副桌宠的全部状态: 进程句柄 (NULL = 当前没在跑)、重试时间、停止期限、失败计数,
 * 以及最后一次写进 control.txt 的值 —— 用它判断要不要重写 (心跳到期或状态变了才写)。
 */
typedef struct SecondaryPet {
    char model_id[BONGO_CAT_ID_CAP];
    SDL_Process *process;
    uint64_t retry_ns, stop_ns;
    unsigned failures;
    bool stopping, control_ready, control_visible, control_pass_through;
} SecondaryPet;
/* 父进程侧的运行时: 固定容量数组, 按需分配 (没有副模型时 app->multi_pet 保持 NULL)。 */
struct BongoCatMultiPetRuntime {
    SecondaryPet pets[BONGO_CAT_ADDITIONAL_MODEL_CAP];
    size_t count;
    uint64_t heartbeat_ns;
};
/* create=true 时顺带把 pets/ 与模型目录建出来, 第一次写入前必须这样调用。 */
static bool control_path(char *target, size_t capacity,
    const BongoCatApp *app, const char *model_id, bool create) {
    char pets[BONGO_CAT_PATH_CAP], directory[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(pets, sizeof(pets), app->primary_state_root,
            "pets") || (create && !bongo_cat_path_create_directory(pets)) ||
        !bongo_cat_multi_pet_state_directory(directory, sizeof(directory),
            app->primary_state_root, model_id) ||
        (create && !bongo_cat_path_create_directory(directory))) return false;
    return bongo_cat_path_join(target, capacity, directory, CONTROL_FILE);
}

/* 把"可见性 + 时间戳 + 穿透"原子写入子进程的 control.txt, 并刷新 control_* 基线。
 * 必须先于 spawn_pet: 子进程启动就读它, 拿不到会走重试宽限, 最后自己退出。
 */
static bool write_control(BongoCatApp *app, SecondaryPet *pet) {
    char path[BONGO_CAT_PATH_CAP], temporary[BONGO_CAT_PATH_CAP];
    int length;
    if (!control_path(path, sizeof(path), app, pet->model_id, true) ||
        (length = snprintf(temporary, sizeof(temporary), "%s.tmp", path)) < 0 ||
        (size_t)length >= sizeof(temporary)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[runtime] Multi-pet control path preparation failed: model=%s",
            pet->model_id);
        return false;
    }
    FILE *file = bongo_cat_file_open(temporary, "wb");
    bool written = file && fprintf(file, "%s %lld %d\n",
        app->session.window.visible ? "visible" : "hidden",
        (long long)time(NULL), app->settings.window.pass_through) > 0;
    if (file && fclose(file) != 0) written = false;
    if (!written || !bongo_cat_file_replace(temporary, path, true)) {
        bongo_cat_file_remove(temporary);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[runtime] Multi-pet control write failed: model=%s path=%s "
            "visible=%d pass_through=%d", pet->model_id, path,
            app->session.window.visible, app->settings.window.pass_through);
        return false;
    }
    pet->control_ready = true;
    pet->control_visible = app->session.window.visible;
    pet->control_pass_through = app->settings.window.pass_through;
    return true;
}

/* 删掉 control.txt 让子进程自行退出 (它读到 missing 后进入重试宽限), 同时清掉 control_ready。 */
static void remove_control(BongoCatApp *app, SecondaryPet *pet) {
    char path[BONGO_CAT_PATH_CAP];
    if (control_path(path, sizeof(path), app, pet->model_id, false))
        bongo_cat_file_remove(path);
    pet->control_ready = false;
}

/* 按模型 id 找槽位; 返回内部指针, remove_pet 或下一次 memmove 之后即失效。 */
static SecondaryPet *find_pet(BongoCatMultiPetRuntime *runtime,
    const char *model_id) {
    for (size_t i = 0; i < runtime->count; ++i)
        if (!strcmp(runtime->pets[i].model_id, model_id))
            return &runtime->pets[i];
    return NULL;
}

/* 摘链并清零尾槽。调用方必须倒序遍历数组, 否则 memmove 之后会跳过元素。 */
static void remove_pet(BongoCatMultiPetRuntime *runtime, size_t index) {
    if (index + 1 < runtime->count)
        memmove(&runtime->pets[index], &runtime->pets[index + 1],
            (runtime->count - index - 1) * sizeof(runtime->pets[0]));
    memset(&runtime->pets[--runtime->count], 0, sizeof(runtime->pets[0]));
}

/* 模型是否仍应显示在桌面上: 总开关与 session 的副模型列表缺一不可。 */
static bool desired(const BongoCatApp *app, const char *model_id) {
    if (!app->settings.model.multiple_pets) return false;
    for (size_t i = 0; i < app->session.additional_model_count; ++i)
        if (!strcmp(app->session.additional_model_ids[i], model_id))
            return true;
    return false;
}

/* 启动子进程。三个标准流都设成 NULL, 不建管道 —— Windows 上从 GUI 启动时没有可继承的
 * 标准句柄, 请求管道反而会让创建失败。
 */
static SDL_Process *create_pet_process(const char *const *args) {
    SDL_PropertiesID properties = SDL_CreateProperties();
    if (!properties) return NULL;
    bool configured = SDL_SetPointerProperty(properties,
            SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args) &&
        SDL_SetNumberProperty(properties,
            SDL_PROP_PROCESS_CREATE_STDIN_NUMBER,
            SDL_PROCESS_STDIO_NULL) &&
        SDL_SetNumberProperty(properties,
            SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER,
            SDL_PROCESS_STDIO_NULL) &&
        SDL_SetNumberProperty(properties,
            SDL_PROP_PROCESS_CREATE_STDERR_NUMBER,
            SDL_PROCESS_STDIO_NULL);
    SDL_Process *process = configured ?
        SDL_CreateProcessWithProperties(properties) : NULL;
    if (!configured) SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
        "[runtime] Multi-pet process properties failed: %s", SDL_GetError());
    SDL_DestroyProperties(properties);
    return process;
}

/* 组装子进程命令行: 可执行文件 + --secondary-pet=<id> + 级联位置偏移 (第 i 个额外偏移
 * 36 * (i+1) 像素), 并把主进程的 storage / nearby / cache 根目录透传下去, 让两边看到同一套数据。
 * 随后清掉历史请求文件, 否则新进程第一帧就会执行上一次残留的指令。
 */
static bool spawn_pet(BongoCatApp *app, SecondaryPet *pet, size_t index,
    uint64_t now) {
    char model_arg[BONGO_CAT_ID_CAP + 20];
    char storage_arg[BONGO_CAT_PATH_CAP + 20], nearby_arg[BONGO_CAT_PATH_CAP + 20];
    char cache_arg[BONGO_CAT_PATH_CAP + 20], position_arg[80];
    int x = app->session.window.x, y = app->session.window.y;
    if (app->window) SDL_GetWindowPosition(app->window, &x, &y);
    snprintf(model_arg, sizeof(model_arg), "--secondary-pet=%s", pet->model_id);
    snprintf(position_arg, sizeof(position_arg), "--secondary-position=%d,%d",
        x + 36 * (int)(index + 1), y + 36 * (int)(index + 1));
    const char *args[8] = {app->executable_path[0] ? app->executable_path :
        BONGO_CAT_EXECUTABLE, model_arg, position_arg, NULL, NULL, NULL, NULL, NULL};
    size_t argument = 3;
    if (app->storage_root[0]) {
        snprintf(storage_arg, sizeof(storage_arg), "--storage-root=%s",
            app->storage_root);
        args[argument++] = storage_arg;
    }
    if (app->nearby_root[0])
        snprintf(nearby_arg, sizeof(nearby_arg), "--nearby-root=%s",
            app->nearby_root), args[argument++] = nearby_arg;
    if (app->cache_root_override[0])
        snprintf(cache_arg, sizeof(cache_arg), "--cache-root=%s",
            app->cache_root_override), args[argument++] = cache_arg;
    bongo_cat_multi_pet_clear_remove_request(app, pet->model_id);
    bongo_cat_multi_pet_clear_pass_through_request(app, pet->model_id);
    bongo_cat_multi_pet_clear_primary_request(app, pet->model_id);
    if (!write_control(app, pet)) {
        pet->failures++;
        pet->retry_ns = now + CHILD_RETRY_NS;
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Unable to prepare additional desktop pet %s",
            pet->model_id);
        return false;
    }
    /* GUI launches may not have inheritable standard handles on Windows. */
    pet->process = create_pet_process(args);
    if (pet->process) {
        pet->stopping = false;
        return true;
    }
    pet->failures++;
    pet->retry_ns = now + CHILD_RETRY_NS;
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
        "Unable to start additional desktop pet %s: %s",
        pet->model_id, SDL_GetError());
    return false;
}

/* 回收退出的子进程并推进停止流程 (倒序遍历, 以便安全摘链)。
 * 非 stopping 的退出算一次失败并安排重试; stopping 的超过 CHILD_STOP_NS 还没走就强杀。
 */
static void reap_pets(BongoCatApp *app, uint64_t now) {
    BongoCatMultiPetRuntime *runtime = app->multi_pet;
    for (size_t i = runtime->count; i > 0; --i) {
        SecondaryPet *pet = &runtime->pets[i - 1];
        int exit_code = -1;
        bool exited = pet->process &&
            SDL_WaitProcess(pet->process, false, &exit_code);
        if (exited) {
            if (!pet->stopping)
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[runtime] Multi-pet process exited unexpectedly: "
                    "model=%s exit_code=%d", pet->model_id, exit_code);
            SDL_DestroyProcess(pet->process);
            pet->process = NULL;
            if (!pet->stopping) {
                pet->failures++;
                pet->retry_ns = now + CHILD_RETRY_NS;
            }
        }
        if (pet->stopping && pet->process && now >= pet->stop_ns) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[runtime] Multi-pet process stop timed out; killing: model=%s",
                pet->model_id);
            SDL_KillProcess(pet->process, true);
            SDL_WaitProcess(pet->process, true, NULL);
            SDL_DestroyProcess(pet->process);
            pet->process = NULL;
        }
        if (pet->stopping && !pet->process) remove_pet(runtime, i - 1);
    }
}

/* 子进程请求"把我移除"时: 先摘掉 session 里的这个模型 (后面的循环据此把它标成 stopping),
 * 再让设置窗口失效重刷。
 */
static void consume_remove_requests(BongoCatApp *app) {
    BongoCatMultiPetRuntime *runtime = app->multi_pet;
    for (size_t i = runtime->count; i > 0; --i) {
        SecondaryPet *pet = &runtime->pets[i - 1];
        if (!pet->process || pet->stopping || !desired(app, pet->model_id) ||
            !bongo_cat_multi_pet_take_remove_request(app, pet->model_id))
            continue;
        bongo_cat_session_remove_model(&app->session, pet->model_id);
        bongo_cat_preferences_invalidate(app->preferences);
    }
}

/* 父进程每帧的状态机。顺序不能随意调换: 先清理会话, 再消费子进程请求 (移除 / 穿透 / 退出),
 * 接着回收退出的进程腾出槽位, 把不再需要的标成 stopping, 最后才做心跳续写与启动。
 * 本轮新增的副模型走同一段循环: 没有槽位就等待, 启动失败 3 次就摘掉模型并提示。
 */
void bongo_cat_multi_pet_primary_update(BongoCatApp *app, uint64_t now) {
    bongo_cat_multi_pet_prune_selection(app);
    if (!app->multi_pet && app->session.additional_model_count)
        app->multi_pet = calloc(1, sizeof(*app->multi_pet));
    if (!app->multi_pet) return;
    BongoCatMultiPetRuntime *runtime = app->multi_pet;
    consume_remove_requests(app);
    bongo_cat_multi_pet_pass_through_requests_update(app);
    bongo_cat_multi_pet_primary_requests_update(app);
    reap_pets(app, now);
    for (size_t i = 0; i < runtime->count; ++i) {
        SecondaryPet *pet = &runtime->pets[i];
        if (!desired(app, pet->model_id) && !pet->stopping) {
            remove_control(app, pet);
            pet->stopping = true;
            pet->stop_ns = now + CHILD_STOP_NS;
        }
    }
    bool heartbeat = !runtime->heartbeat_ns ||
        now - runtime->heartbeat_ns >= CONTROL_HEARTBEAT_NS;
    for (size_t i = 0; i < app->session.additional_model_count; ++i) {
        const char *id = app->session.additional_model_ids[i];
        SecondaryPet *pet = find_pet(runtime, id);
        if (!pet && runtime->count < BONGO_CAT_ADDITIONAL_MODEL_CAP) {
            pet = &runtime->pets[runtime->count++];
            snprintf(pet->model_id, sizeof(pet->model_id), "%s", id);
        }
        if (!pet || pet->stopping) continue;
        if (heartbeat || !pet->control_ready ||
            pet->control_visible != app->session.window.visible ||
            pet->control_pass_through != app->settings.window.pass_through)
            write_control(app, pet);
        if (!pet->process && pet->failures < 3 && now >= pet->retry_ns)
            spawn_pet(app, pet, i, now);
        if (!pet->process && pet->failures >= 3) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "[runtime] Multi-pet model removed after launch failures: "
                "model=%s failures=%u", pet->model_id, pet->failures);
            bongo_cat_session_remove_model(&app->session, pet->model_id);
            remove_control(app, pet);
            pet->stopping = true;
            if (app->preferences) bongo_cat_preferences_notice_show(app,
                bongo_cat_i18n_get(app->i18n, "native.modelLoadFailed",
                    "Unable to display this model"), true);
        }
    }
    if (heartbeat) runtime->heartbeat_ns = now;
}

/* 退出: 先删掉所有 control.txt 让子进程有机会自己走, 500 ms 是全部子进程共享的总预算,
 * 超时强杀, 最后释放运行时并把 app->multi_pet 置空。
 */
void bongo_cat_multi_pet_shutdown(BongoCatApp *app) {
    if (!app || !app->multi_pet) return;
    BongoCatMultiPetRuntime *runtime = app->multi_pet;
    for (size_t i = 0; i < runtime->count; ++i)
        remove_control(app, &runtime->pets[i]);
    uint64_t deadline = SDL_GetTicksNS() + 500000000ull;
    for (size_t i = 0; i < runtime->count; ++i) {
        SecondaryPet *pet = &runtime->pets[i];
        while (pet->process && SDL_GetTicksNS() < deadline &&
            !SDL_WaitProcess(pet->process, false, NULL)) SDL_Delay(10);
        if (pet->process && !SDL_WaitProcess(pet->process, false, NULL)) {
            SDL_KillProcess(pet->process, true);
            SDL_WaitProcess(pet->process, true, NULL);
        }
        if (pet->process) SDL_DestroyProcess(pet->process);
    }
    free(runtime);
    app->multi_pet = NULL;
}
