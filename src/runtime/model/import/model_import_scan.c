/*
 * 有预算的递归目录扫描: 从 root 逐层找出"自身就是包根"的目录, 每个交给 visitor 处理一次; 预算
 * 用完就安静收尾, 语义是"结果可能不完整"而不是失败(契约见 model_import.h 的 scan_budget)。
 * 时序: 渐进式导入与附近模型扫描都从这里进来; 根的直接子目录先按路径排序, 之后广度优先。
 * 契约: visitor 只在被识别为包根的节点上调用, 命中节点不再往下展开; visitor 返回的错误码不会
 * 中断遍历, 只被记为第一个错误并在最后返回, 且 visitor 自身耗时不计入预算 —— 预算量的是"找包"
 * 的开销。最容易改错: 节点数、条目数、子目录数三处上限都只在 limited 上留痕, 不影响返回码。
 */
#include "model_import.h"
#include "model_import_probe.h"
#include "bongo_cat/path.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 扫描资源上限: 单目录最多收 64 个子目录与 256 个条目, 整次扫描最多 256 个节点, 队列里最深排到
 * 深度 3(根算 0, 深度 3 的节点仍会被识别但不再展开), 墙钟预算 2 秒。超限一律降级, 不报错。 */
#define IMPORT_CHILD_CAP 64
#define IMPORT_ENTRY_CAP 256
#define IMPORT_NODE_CAP 256
#define IMPORT_SCAN_DEPTH 3
#define IMPORT_SCAN_BUDGET_NS 2000000000ull

/* 单个目录的子目录清单: entries 统计目录里出现的条目总数(含隐藏项), count 是收集到的子目录数;
 * 任一上限触发就置 limited 并提前结束枚举(提前结束仍算枚举成功)。 */
typedef struct ChildList {
    char paths[IMPORT_CHILD_CAP][BONGO_CAT_PATH_CAP];
    size_t count, entries;
    bool limited;
} ChildList;

/* 每个被处理的节点临时分配的堆工作区: 内含一个约百 KB 的 discovery(候选全是定长路径), 不能放
 * 栈上; 节点处理完立刻 free, 因此同时存活的工作区数量与扫描深度同阶。 */
typedef struct ImportWorkspace {
    BongoCatImportDiscovery discovery;
    ChildList children;
} ImportWorkspace;

/* 扫描状态: deadline 是绝对时刻(SDL_GetTicksNS 的单调基准), visitor 花掉的墙钟时间会补回
 * deadline, 使预算只吃在扫描自身; limited 只用于收尾日志, 不参与返回码。 */
typedef struct ImportScan {
    BongoCatImportVisitor visitor;
    void *userdata;
    BongoCatError *error;
    size_t nodes;
    uint64_t deadline;
    bool limited;
    bool diagnostic;
} ImportScan;

/* 广度优先队列: 定长数组加 head 下标, 不做动态增长, 满了就置 limited 停止追加。 */
typedef struct ImportQueue {
    char paths[IMPORT_NODE_CAP][BONGO_CAT_PATH_CAP];
    unsigned char depths[IMPORT_NODE_CAP];
    size_t head, count;
} ImportQueue;

/* 枚举回调: 只收目录并跳过隐藏项; 条目或子目录超限时置 limited 并返回 SUCCESS 提前结束本次
 * 枚举(见 path.h: SUCCESS 表示"够了", 枚举整体仍算成功)。不做路径规范化, 后续按字符串排序。 */
static BongoCatPathVisit collect_child(void *userdata,
    const char *dirname, const char *name) {
    ChildList *list = userdata;
    if (++list->entries > IMPORT_ENTRY_CAP || list->count >= IMPORT_CHILD_CAP) {
        list->limited = true; return BONGO_CAT_PATH_SUCCESS;
    }
    if (name[0] == '.') return BONGO_CAT_PATH_CONTINUE;
    char path[BONGO_CAT_PATH_CAP];
    if (bongo_cat_path_join(path, sizeof(path), dirname, name) &&
        bongo_cat_path_is_dir(path))
        snprintf(list->paths[list->count++], BONGO_CAT_PATH_CAP, "%s", path);
    return BONGO_CAT_PATH_CONTINUE;
}

/* 排序让同一目录树每次扫描的遍历顺序一致(变体编号、日志与导入顺序都可复现)。Windows 文件系统
 * 大小写不敏感, 用不区分大小写的比较才与用户看到的顺序一致。 */
static int compare_path(const void *left, const void *right) {
#ifdef _WIN32
    return SDL_strcasecmp(left, right);
#else
    return strcmp(left, right);
#endif
}

/* 预算检查: 同时管节点数与墙钟, 超限时置 limited 并返回 false, 由调用方安静收工。 */
static bool within_budget(ImportScan *scan) {
    if (scan->nodes >= IMPORT_NODE_CAP || SDL_GetTicksNS() >= scan->deadline) {
        scan->limited = true; return false;
    }
    scan->nodes++; return true;
}

/* 按已排好的顺序把子目录追加到队列尾部; 队列满时置 limited 并停止追加, 不覆盖已有节点。 */
static void enqueue_children(ImportScan *scan, ImportQueue *queue,
    const ChildList *children, int depth) {
    for (size_t i = 0; i < children->count; ++i) {
        if (queue->count >= IMPORT_NODE_CAP) {
            scan->limited = true; return;
        }
        snprintf(queue->paths[queue->count], BONGO_CAT_PATH_CAP, "%s",
            children->paths[i]);
        queue->depths[queue->count++] = (unsigned char)depth;
    }
}

/* 处理一个节点: 先做精确探测(节点扫描用 FALLBACK 策略 —— 坏包在这里不致命, 会退到子目录继续
 * 找), 命中就回调 visitor 并直接返回其结果, 此时不再展开子目录; 未命中且还有深度余量时枚举
 * 子目录入队。返回值 OK 表示"这个节点处理完了"(含预算耗尽), 非 OK 来自 visitor 或枚举。
 * work 在每条返回路径上都要 free。 */
static BongoCatResult scan_node(ImportScan *scan, ImportQueue *queue,
    const char *source, int depth) {
    if (!within_budget(scan)) return BONGO_CAT_OK;
    uint64_t started = SDL_GetTicksNS();
    if (scan->diagnostic) SDL_Log(
        "[runtime] Model import scan node started: node=%llu depth=%d "
        "path=%s", (unsigned long long)scan->nodes, depth, source);
    ImportWorkspace *work = calloc(1, sizeof(*work));
    if (!work) {
        bongo_cat_error_set(scan->error, BONGO_CAT_ERROR_MEMORY,
            "Cannot allocate model import scan workspace");
        return BONGO_CAT_ERROR_MEMORY;
    }
    BongoCatError local = {0};
    BongoCatImportFormat format = BONGO_CAT_IMPORT_MVER;
    int found = bongo_cat_import_probe_exact(source, &work->discovery,
        &format, BONGO_CAT_IMPORT_PROBE_FALLBACK, scan->diagnostic, &local);
    if (found > 0) {
        if (scan->diagnostic) SDL_Log(
            "[runtime] Model import scan node discovered: format=%s "
            "candidates=%llu elapsed_ms=%.1f path=%s",
            bongo_cat_import_format_name(format),
            (unsigned long long)work->discovery.count,
            (SDL_GetTicksNS() - started) / 1000000.0, source);
        /* 把 visitor 花掉的时间补回 deadline: 预算限制的是扫描本身, 不惩罚真正干活的回调。 */
        uint64_t visitor_started = SDL_GetTicksNS();
        BongoCatResult result = scan->visitor(scan->userdata, source,
            &work->discovery, scan->error);
        uint64_t finished = SDL_GetTicksNS();
        uint64_t elapsed = finished - visitor_started;
        if (scan->deadline <= UINT64_MAX - elapsed) scan->deadline += elapsed;
        else scan->deadline = UINT64_MAX;
        if (scan->diagnostic) SDL_Log(
            "[runtime] Model import scan node collected: result=%d "
            "elapsed_ms=%.1f path=%s", (int)result,
            (finished - started) / 1000000.0, source);
        free(work); return result;
    }
    if (depth < IMPORT_SCAN_DEPTH) {
        bool enumerated = bongo_cat_path_enumerate(source, collect_child,
            &work->children);
        if (!enumerated) {
            if (!scan->error || scan->error->code == BONGO_CAT_OK)
                bongo_cat_error_set(scan->error, BONGO_CAT_ERROR_IO,
                    "Cannot enumerate model directory: %s", source);
            BongoCatResult result = scan->error && scan->error->code
                ? scan->error->code : BONGO_CAT_ERROR_IO;
            free(work);
            return result;
        }
        if (work->children.limited) scan->limited = true;
        qsort(work->children.paths, work->children.count,
            sizeof(work->children.paths[0]), compare_path);
        enqueue_children(scan, queue, &work->children, depth + 1);
    }
    if (scan->diagnostic) SDL_Log(
        "[runtime] Model import scan node completed: children=%llu "
        "elapsed_ms=%.1f path=%s",
        (unsigned long long)work->children.count,
        (SDL_GetTicksNS() - started) / 1000000.0, source);
    free(work); return BONGO_CAT_OK;
}

/* 扫描主入口: root 的直接子目录先入队(深度 1), 然后广度优先跑到队列空或预算耗尽。返回 OK 不
 * 代表结果完整(看 limited 与收尾日志); 只有根目录枚举失败或 visitor 报错才返回非 OK, 且返回
 * 第一个错误码 —— 单个坏节点不会中断后续遍历。 */
static BongoCatResult scan_with_budget(const char *root,
    BongoCatImportVisitor visitor, void *userdata, uint64_t budget_ns,
    bool diagnostic, BongoCatError *error) {
    if (!root || !visitor) return BONGO_CAT_ERROR_ARGUMENT;
    ImportWorkspace *work = calloc(1, sizeof(*work));
    ImportQueue *queue = calloc(1, sizeof(*queue));
    if (!work || !queue) {
        free(work);
        free(queue);
        bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY,
            "Cannot allocate model import directory scan");
        return BONGO_CAT_ERROR_MEMORY;
    }
    uint64_t now = SDL_GetTicksNS();
    uint64_t deadline = budget_ns <= UINT64_MAX - now
        ? now + budget_ns : UINT64_MAX;
    ImportScan scan = {
        visitor, userdata, error, 0, deadline, false, diagnostic
    };
    BongoCatResult result = BONGO_CAT_OK;
    if (diagnostic) SDL_Log(
        "[runtime] Model import scan started: budget_ms=%.1f path=%s",
        budget_ns / 1000000.0, root);
    if (bongo_cat_path_enumerate(root, collect_child, &work->children)) {
        if (work->children.limited) scan.limited = true;
        qsort(work->children.paths, work->children.count,
            sizeof(work->children.paths[0]), compare_path);
        enqueue_children(&scan, queue, &work->children, 1);
        while (queue->head < queue->count) {
            size_t current = queue->head++;
            BongoCatResult item = scan_node(&scan, queue,
                queue->paths[current], queue->depths[current]);
            if (item != BONGO_CAT_OK && result == BONGO_CAT_OK) result = item;
        }
    } else {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "Cannot enumerate model directory: %s", root);
        result = error && error->code ? error->code : BONGO_CAT_ERROR_IO;
    }
    if (scan.limited) SDL_Log(
        "Model folder scan reached its directory budget");
    if (diagnostic) SDL_Log(
        "[runtime] Model import scan completed: result=%d nodes=%llu "
        "queued=%llu limited=%d elapsed_ms=%.1f path=%s", (int)result,
        (unsigned long long)scan.nodes, (unsigned long long)queue->count,
        scan.limited, (SDL_GetTicksNS() - now) / 1000000.0, root);
    free(queue); free(work); return result;
}

/* 调用方自带预算的入口: 附近模型扫描用它把预算压到 500 ms。 */
BongoCatResult bongo_cat_import_scan_budget(const char *root,
    BongoCatImportVisitor visitor, void *userdata, uint64_t budget_ns,
    BongoCatError *error) {
    return scan_with_budget(root, visitor, userdata, budget_ns, false, error);
}

/* 默认 2 秒预算的普通入口: 单个节点的错误会成为返回值, 供交互式导入使用。 */
BongoCatResult bongo_cat_import_scan(const char *root,
    BongoCatImportVisitor visitor, void *userdata, BongoCatError *error) {
    return scan_with_budget(root, visitor, userdata,
        IMPORT_SCAN_BUDGET_NS, false, error);
}

/* 与 scan 相同但打开逐节点日志: 用户点导入时用它把每步耗时写进日志, 便于定位卡顿。 */
BongoCatResult bongo_cat_import_scan_diagnostic(const char *root,
    BongoCatImportVisitor visitor, void *userdata, BongoCatError *error) {
    return scan_with_budget(root, visitor, userdata,
        IMPORT_SCAN_BUDGET_NS, true, error);
}
