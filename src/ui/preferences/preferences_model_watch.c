/*
 * 模型目录的变更监视。
 *
 * 用轮询而不是文件系统通知: 通知 API 在三个平台上差异太大, 而且导入过程本身就会
 * 反复改这个目录, 通知风暴反而难处理。这里 750ms 轮询一次, 只统计"有多少个子目录"
 * 以及两个哈希 (sum / xor), 拿这三个数当目录指纹 —— 不读文件内容, 一次统计很便宜。
 * 同时用 sum 和 xor 是因为它们失效方式不同: 改名/增删会同时改变两者, 而单纯的顺序
 * 变化只影响 xor; 两个都留着可以把"几乎肯定变了"和"可能没变"区分得更好。
 * 轮询只在"模型页可见"时才跑, 其它时候立刻熄火 (见函数开头的 active 判定)。
 */
#include "preferences_state.h"
#include "bongo_cat/path.h"

#include <SDL3/SDL.h>

/*
 * 750ms 是"用户感知不到延迟"和"不做无谓 IO"之间的折中; 导入期间整段跳过,
 * 否则导入自己写进去的文件会不停触发目录刷新。
 */
#define MODEL_DIRECTORY_WATCH_INTERVAL_NS 750000000ull

typedef struct ModelDirectorySnapshot {
    uint64_t sum;
    uint64_t xor_value;
    size_t count;
    bool available;
} ModelDirectorySnapshot;

/*
 * splitmix64 的收尾混合: 把输入位打散, 让"名字只差一点"的两个目录也能得到差别
 * 很大的哈希, 否则 sum 容易被相互抵消。
 */
static uint64_t mix_hash(uint64_t value) {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ull;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}

/* FNV-1a: 短字符串够用且无需初始化表 */
static uint64_t name_hash(const char *name) {
    uint64_t value = 1469598103934665603ull;
    for (const unsigned char *cursor = (const unsigned char *)name;
         cursor && *cursor; ++cursor) {
        value ^= *cursor;
        value *= 1099511628211ull;
    }
    return value;
}

/*
 * 只数目录、跳过点开头的条目 (macOS 的 .DS_Store、版本控制的残留等):
 * 把这些算进去会让指纹自己抖动, 变成无休止的刷新。
 */
static BongoCatPathVisit snapshot_item(void *userdata,
    const char *directory, const char *name) {
    ModelDirectorySnapshot *snapshot = userdata;
    if (!name || name[0] == '.') return BONGO_CAT_PATH_CONTINUE;
    char path[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(path, sizeof(path), directory, name) ||
        !bongo_cat_path_is_dir(path)) return BONGO_CAT_PATH_CONTINUE;
    uint64_t hash = name_hash(name);
    snapshot->sum += mix_hash(hash);
    snapshot->xor_value ^= mix_hash(hash ^ 0x9e3779b97f4a7c15ull);
    snapshot->count++;
    return BONGO_CAT_PATH_CONTINUE;
}

static ModelDirectorySnapshot directory_snapshot(const char *root) {
    ModelDirectorySnapshot snapshot = {0};
    snapshot.available = root && bongo_cat_path_is_dir(root) &&
        bongo_cat_path_enumerate(root, snapshot_item, &snapshot);
    return snapshot;
}

/*
 * 每个 UI 帧调一次 (由 render 路径驱动, 不是独立线程)。
 * 三点值得注意:
 * - 只有模型页可见时才工作, 其它页面/窗口关掉时立刻把 active 清掉, 下次回来
 * 重新计时;
 * - 刚激活时 due_ns 归零, 于是第一帧立刻扫一次 (而不是等 750ms), 用户切到这一页
 * 马上就能看到最新的模型列表;
 * - 第一次扫描只记录不算"变了" (known 为假), 否则每次开窗都会白白触发一次目录
 * 刷新。
 * 导入进行中直接跳过: 那期间目录本来就在变, 刷新没有意义, 而且会和导入抢目录。
 */
void bongo_cat_preferences_model_watch(BongoCatPreferences *value,
    uint64_t now) {
    if (!value) return;
    bool active = value->visible && value->page == 1 && value->window;
    if (!active) {
        value->model_directory_watch_active = false;
        return;
    }
    if (!value->model_directory_watch_active) {
        value->model_directory_watch_active = true;
        if (!value->model_directory_watch_known)
            value->model_directory_watch_refresh = true;
        value->model_directory_watch_due_ns = 0;
    }
    if (value->model_directory_watch_due_ns > now) return;
    value->model_directory_watch_due_ns = now +
        MODEL_DIRECTORY_WATCH_INTERVAL_NS;
    if (bongo_cat_preferences_import_status(value->import_dialog,
            NULL, NULL, NULL)) return;

    ModelDirectorySnapshot current = directory_snapshot(
        value->app->models_root);
    bool changed = value->model_directory_watch_known &&
        (value->model_directory_watch_available != current.available ||
         value->model_directory_watch_count != current.count ||
         value->model_directory_watch_sum != current.sum ||
         value->model_directory_watch_xor != current.xor_value);
    value->model_directory_watch_known = true;
    value->model_directory_watch_available = current.available;
    value->model_directory_watch_count = current.count;
    value->model_directory_watch_sum = current.sum;
    value->model_directory_watch_xor = current.xor_value;
    if (changed || value->model_directory_watch_refresh) {
        value->model_directory_watch_refresh = false;
        bongo_cat_app_request_model_refresh(value->app);
    }
}
