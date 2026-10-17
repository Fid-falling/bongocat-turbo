/*
 * 配置持久化 (settings.json / session.json) 的"脏了也不立刻写"策略。
 *
 * 为什么用内容哈希而不是 dirty 标记: 拖窗口/滚轮缩放会高频改 session, 直接落盘会把
 * IO 打满; 哈希还天然处理了"改回原值"的情况 —— 值没变就什么都不做。观察到变化后从
 * 那一刻起延迟 300ms 写, 期间继续变就顺延; 写失败 1s 后重试。
 *
 * 两个 blocked 开关: 配置文件解析失败时先改名成 *.rejected[.N] 保留证据, 再当默认值
 * 用; 如果连改名都失败 (权限/被占用), 就置 blocked —— 绝不用默认值覆盖用户文件。
 *
 * 副宠进程不写 settings (只写自己的 session); SMOKE 运行完全不落盘, 保证 CI 可复现。
 */
#include "runtime.h"
#include "bongo_cat/log.h"
#include "bongo_cat/path.h"

#include <stdio.h>
#include <string.h>

#define SAVE_DELAY_NS 300000000ull
#define RETRY_DELAY_NS 1000000000ull

/* FNV-1a, 只用来判断"内容是否变化", 不是安全用途。逐字段哈希结构体 (含 padding),
   所以结构体成员增删都会改变哈希 —— 这正是想要的。 */
static uint64_t hash_bytes(uint64_t hash, const void *data, size_t size) {
    const unsigned char *bytes = data;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

static uint64_t settings_hash(const BongoCatSettings *settings) {
    uint64_t hash = 1469598103934665603ull;
#define HASH_FIELD(field) hash = hash_bytes(hash, &(field), sizeof(field))
    HASH_FIELD(settings->model);
    HASH_FIELD(settings->window);
    HASH_FIELD(settings->app);
    HASH_FIELD(settings->shortcuts);
    /* 音效设置也是用户点一下就变的交互, 必须逐字段进哈希 —— 漏掉就会出现
       "关掉一个音效、重启又全开" (这个坑在别的字段上踩过)。
       刻意不 HASH_FIELD(settings->sound): 那个结构里带着 256 条定长 id (约 128KB),
       每帧哈希一遍纯属浪费; 标量 + 实际条目已经覆盖全部可改内容。 */
    HASH_FIELD(settings->sound.volume);
    HASH_FIELD(settings->sound.muted);
    HASH_FIELD(settings->sound.disabled_count);
    size_t sound_count = settings->sound.disabled_count;
    if (sound_count > BONGO_CAT_SOUND_DISABLED_CAP)
        sound_count = BONGO_CAT_SOUND_DISABLED_CAP;
    hash = hash_bytes(hash, settings->sound.disabled,
        sound_count * sizeof(settings->sound.disabled[0]));
    HASH_FIELD(settings->behavior_shortcut_count);
    size_t behavior_count = settings->behavior_shortcut_count;
    if (behavior_count > BONGO_CAT_BEHAVIOR_BINDING_CAP)
        behavior_count = BONGO_CAT_BEHAVIOR_BINDING_CAP;
    hash = hash_bytes(hash, settings->behavior_shortcuts,
        behavior_count * sizeof(settings->behavior_shortcuts[0]));
    HASH_FIELD(settings->model_label_count);
    size_t model_count = settings->model_label_count;
    if (model_count > BONGO_CAT_MODEL_CAP) model_count = BONGO_CAT_MODEL_CAP;
    hash = hash_bytes(hash, settings->model_labels,
        model_count * sizeof(settings->model_labels[0]));
    HASH_FIELD(settings->removed_model_count);
    size_t removed_count = settings->removed_model_count;
    if (removed_count > BONGO_CAT_MODEL_CAP)
        removed_count = BONGO_CAT_MODEL_CAP;
    hash = hash_bytes(hash, settings->removed_models,
        removed_count * sizeof(settings->removed_models[0]));
    /* 隐藏模型与随机/顺序切换的参与集合也是用户设置, 而且都是"点一下就变"的交互。
       它们不在 model/window/app 这些大结构里, 漏掉就会出现"改完不落盘、重启即丢"。 */
    HASH_FIELD(settings->hidden_model_count);
    size_t hidden_count = settings->hidden_model_count;
    if (hidden_count > BONGO_CAT_MODEL_CAP) hidden_count = BONGO_CAT_MODEL_CAP;
    hash = hash_bytes(hash, settings->hidden_models,
        hidden_count * sizeof(settings->hidden_models[0]));
    HASH_FIELD(settings->random_disabled_count);
    size_t disabled_count = settings->random_disabled_count;
    if (disabled_count > BONGO_CAT_RANDOM_DISABLED_CAP)
        disabled_count = BONGO_CAT_RANDOM_DISABLED_CAP;
    hash = hash_bytes(hash, settings->random_disabled,
        disabled_count * sizeof(settings->random_disabled[0]));
    HASH_FIELD(settings->extensions_json);
#undef HASH_FIELD
    return hash;
}

static uint64_t session_hash(const BongoCatSessionState *session) {
    uint64_t hash = 1469598103934665603ull;
#define HASH_FIELD(field) hash = hash_bytes(hash, &(field), sizeof(field))
    HASH_FIELD(session->window.visible);
    HASH_FIELD(session->window.position_known);
    HASH_FIELD(session->window.scale_percent);
    HASH_FIELD(session->window.opacity_percent);
    HASH_FIELD(session->window.x);
    HASH_FIELD(session->window.y);
    HASH_FIELD(session->window.width);
    HASH_FIELD(session->window.height);
    HASH_FIELD(session->window.content_width);
    HASH_FIELD(session->window.content_height);
    HASH_FIELD(session->active_model_id);
    HASH_FIELD(session->last_update_check_day);
    HASH_FIELD(session->last_update_check_version);
    HASH_FIELD(session->available_update_version);
    HASH_FIELD(session->additional_model_count);
    size_t model_count = session->additional_model_count;
    if (model_count > BONGO_CAT_ADDITIONAL_MODEL_CAP)
        model_count = BONGO_CAT_ADDITIONAL_MODEL_CAP;
    hash = hash_bytes(hash, session->additional_model_ids,
        model_count * sizeof(session->additional_model_ids[0]));
    HASH_FIELD(session->active_behavior_count);
    size_t behavior_count = session->active_behavior_count;
    if (behavior_count > BONGO_CAT_BEHAVIOR_BINDING_CAP)
        behavior_count = BONGO_CAT_BEHAVIOR_BINDING_CAP;
    hash = hash_bytes(hash, session->active_behaviors,
        behavior_count * sizeof(session->active_behaviors[0]));
#undef HASH_FIELD
    return hash;
}

/* 从 path.rejected 开始往后找一个没被占用的名字 (最多 1000 个), 用改名而不是删除:
   用户的数据不能因为一次解析失败就消失。 */
static bool reject_configuration(const char *path, char *rejected,
    size_t capacity) {
    for (unsigned index = 0; index < 1000; ++index) {
        int length = index ? snprintf(rejected, capacity, "%s.rejected.%u",
            path, index) : snprintf(rejected, capacity, "%s.rejected", path);
        if (length < 0 || (size_t)length >= capacity) return false;
        if (bongo_cat_path_is_file(rejected) ||
            bongo_cat_path_is_dir(rejected)) continue;
        return bongo_cat_path_rename(path, rejected);
    }
    return false;
}

/* 只有"格式错误 + 文件确实存在"才走保留路径; 其它错误 (IO/权限) 把文件留在原地并置
   blocked, 免得之后用默认值把它覆盖掉。 */
static void handle_rejected_configuration(const char *description,
    const char *path, BongoCatResult result, bool *blocked) {
    if (result != BONGO_CAT_ERROR_FORMAT || !bongo_cat_path_is_file(path)) {
        *blocked = bongo_cat_path_is_file(path);
        return;
    }
    char rejected[BONGO_CAT_PATH_CAP];
    if (reject_configuration(path, rejected, sizeof(rejected))) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "%s preserved as rejected file: %s", description, rejected);
        return;
    }
    *blocked = true;
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
        "%s will not be overwritten because it could not be preserved: %s",
        description, path);
}

/* 读两份配置。settings 与 session 各自的 valid/blocked 结果决定后面还写不写盘。
   语言只在"没有有效 settings"时才按系统区域猜一次: 用户显式选过的语言必须优先于系统
   区域, 否则换个系统语言就会把用户的选择冲掉。 */
void bongo_cat_config_store_load(BongoCatApp *app) {
    app->settings_store_valid = false;
    app->session_store_valid = false;
    app->settings_store_blocked = false;
    app->session_store_blocked = false;
    BongoCatError error = {0};
    BongoCatResult loaded = bongo_cat_settings_load(
        app->settings_path, &app->settings, &error);
    /* 校验纠正过的项 (目前只有渲染精度) 在这里落到日志: 核心层没有 SDL 日志,
       把描述攒在校验器里, 由运行时统一打一条 WARN 并清空。 */
    {
        const char *note = bongo_cat_settings_validation_note();
        if (note && note[0]) {
            SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE, "%s", note);
            bongo_cat_settings_clear_validation_note();
        }
    }
    app->settings_store_valid = loaded == BONGO_CAT_OK &&
        bongo_cat_path_is_file(app->settings_path);
    bool language_detected = false;
    if (!app->settings_store_valid) {
        /* System locale is only a first-run/recovery fallback; a valid
           settings file always wins so an explicit user choice persists. */
        BongoCatLanguage detected_language = app->settings.app.language;
        language_detected = bongo_cat_system_language(&detected_language);
        if (language_detected) app->settings.app.language = detected_language;
    }
    if (language_detected)
        SDL_Log("[runtime] Initial language selected from system locale: %s",
            bongo_cat_language_name(app->settings.app.language));
    if (loaded != BONGO_CAT_OK) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Settings ignored: %s",
            error.message);
        handle_rejected_configuration("Settings", app->settings_path,
            loaded, &app->settings_store_blocked);
    }
    error = (BongoCatError){0};
    loaded = bongo_cat_session_load(
        app->session_path, &app->session, &error);
    app->session_store_valid = loaded == BONGO_CAT_OK &&
        bongo_cat_path_is_file(app->session_path);
    if (loaded != BONGO_CAT_OK) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Session ignored: %s",
            error.message);
        handle_rejected_configuration("Session", app->session_path,
            loaded, &app->session_store_blocked);
    }
    /* 没有可用配置时把 saved_hash 取反: 主循环第一轮就会认为"与磁盘不一致", 于是把
       默认设置写出来, 下一次启动就有文件可读了。 */
    uint64_t settings = settings_hash(&app->settings);
    uint64_t session = session_hash(&app->session);
    app->settings_observed_hash = settings;
    app->session_observed_hash = session;
    app->settings_saved_hash = app->settings_store_valid ||
        app->settings_store_blocked ? settings : ~settings;
    app->session_saved_hash = app->session_store_valid ||
        app->session_store_blocked ? session : ~session;
}

/* 保存前必过 validate: 内存里的值可能被外部 JSON / 命令行塞进越界内容。写成功后同步
   三个哈希 (saved / observed / store_valid)。 */
static bool save_settings(BongoCatApp *app) {
    bongo_cat_settings_validate(&app->settings);
    uint64_t hash = settings_hash(&app->settings);
    BongoCatError error = {0};
    if (bongo_cat_settings_save(app->settings_path,
        &app->settings, &error) != BONGO_CAT_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", error.message);
        return false;
    }
    app->settings_saved_hash = hash;
    app->settings_observed_hash = hash;
    app->settings_store_valid = true;
    return true;
}

static bool save_session(BongoCatApp *app) {
    bongo_cat_session_validate(&app->session);
    uint64_t hash = session_hash(&app->session);
    BongoCatError error = {0};
    if (bongo_cat_session_save(app->session_path,
        &app->session, &error) != BONGO_CAT_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", error.message);
        return false;
    }
    app->session_saved_hash = hash;
    app->session_observed_hash = hash;
    app->session_store_valid = true;
    return true;
}

/* 主循环末尾每轮调用。注意副宠只允许写 session —— 两个进程同时写 settings.json 会
   互相覆盖。 */
void bongo_cat_config_store_update(BongoCatApp *app, uint64_t now) {
    if (!app || app->smoke || !app->settings_path[0] || !app->session_path[0]) return;
    uint64_t settings = settings_hash(&app->settings);
    uint64_t session = session_hash(&app->session);
    if (!app->secondary_pet && !app->settings_store_blocked) {
        if (settings != app->settings_observed_hash) {
            app->settings_observed_hash = settings;
            app->settings_save_due_ns = now + SAVE_DELAY_NS;
        } else if (settings != app->settings_saved_hash &&
            !app->settings_save_due_ns)
            app->settings_save_due_ns = now + SAVE_DELAY_NS;
    }
    if (!app->session_store_blocked) {
        if (session != app->session_observed_hash) {
            app->session_observed_hash = session;
            app->session_save_due_ns = now + SAVE_DELAY_NS;
        } else if (session != app->session_saved_hash &&
            !app->session_save_due_ns)
            app->session_save_due_ns = now + SAVE_DELAY_NS;
    }
    if (!app->secondary_pet && !app->settings_store_blocked &&
        app->settings_save_due_ns &&
        now >= app->settings_save_due_ns)
        app->settings_save_due_ns = save_settings(app)
            ? 0 : now + RETRY_DELAY_NS;
    if (!app->session_store_blocked && app->session_save_due_ns &&
        now >= app->session_save_due_ns)
        app->session_save_due_ns = save_session(app)
            ? 0 : now + RETRY_DELAY_NS;
}

/* 关闭路径上的最后一次写: 不看延迟, 直接把"与磁盘不一致"的都写掉并让哈希对齐, 免得
   退出前又排出一个 pending 保存。 */
void bongo_cat_config_store_flush(BongoCatApp *app) {
    if (!app || app->smoke || !app->settings_path[0] || !app->session_path[0]) return;
    uint64_t settings = settings_hash(&app->settings);
    uint64_t session = session_hash(&app->session);
    if (!app->secondary_pet && !app->settings_store_blocked &&
        settings != app->settings_saved_hash) save_settings(app);
    if (!app->session_store_blocked &&
        session != app->session_saved_hash) save_session(app);
    app->settings_observed_hash = settings_hash(&app->settings);
    app->session_observed_hash = session_hash(&app->session);
    app->settings_save_due_ns = app->session_save_due_ns = 0;
}
