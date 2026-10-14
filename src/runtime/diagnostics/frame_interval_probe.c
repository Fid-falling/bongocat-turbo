/*
 * "主循环有没有在出帧"的量化探针 (--ci-frame-series 专用)。
 *
 * 为什么需要它: 帧内容诊断 (frame_audit.c) 只在"真的往窗口上呈现"的那条路径上记录,
 * 而 OpenGL 的透明背景走的是代理分层呈现 —— 那条路径下后台缓冲并不逐帧提交, 于是
 * frame-series.csv 里看不到帧, 用它量出来的间隔会把"主循环一直在正常出帧"误报成
 * "整段没有帧"。排查"切换模型时一帧都不出"这种事时, 需要的恰恰是"主循环多久画一次"。
 *
 * 口径: 每次主循环真的画出并提交了一帧 (present=true) 就记一次时间戳, 上报相邻两次的
 * 最大间隔与分位数。它是纯观测设施: 只在 --ci-frame-series 打开时工作, 不改变任何渲染
 * 或状态; 也不做任何 I/O 以外的分配。
 *
 * 写盘: <state_root>/frame-interval.txt, 每次上报覆盖写 (只保留最近一段窗口的统计)。
 */
#include "runtime.h"
#include "bongo_cat/file.h"
#include "bongo_cat/path.h"

#include <SDL3/SDL_timer.h>
#include <stdio.h>
#include <string.h>

/* 采样窗口: 只保留最近 4096 个间隔。8 秒的 CI 运行最多几百帧, 足够; 长运行也不会无限涨。 */
#define FRAME_INTERVAL_CAP 4096

typedef struct FrameIntervalProbe {
    uint64_t last_ns;
    uint64_t gaps_ns[FRAME_INTERVAL_CAP];
    size_t count;
    uint64_t slowest_ns;
    uint64_t slowest_at_ns;
    uint64_t total_frames;
} FrameIntervalProbe;

static FrameIntervalProbe probe;

/* 第 p 分位 (0..1) 的间隔, 单位毫秒; 没有样本时返回 -1。内部用插入排序的副本, 4096 个
 * uint64 的排序开销可忽略, 而它只在写报告时发生, 不在逐帧路径上。 */
static double percentile_ms(double p) {
    if (!probe.count) return -1.0;
    static uint64_t sorted[FRAME_INTERVAL_CAP];
    memcpy(sorted, probe.gaps_ns, probe.count * sizeof(sorted[0]));
    for (size_t i = 1; i < probe.count; ++i) {
        uint64_t value = sorted[i];
        size_t j = i;
        while (j > 0 && sorted[j - 1] > value) {
            sorted[j] = sorted[j - 1];
            --j;
        }
        sorted[j] = value;
    }
    size_t index = (size_t)(p * (double)(probe.count - 1) + 0.5);
    if (index >= probe.count) index = probe.count - 1;
    return (double)sorted[index] / 1000000.0;
}

/* 把当前统计写到 state_root/frame-interval.txt。覆盖写, 内容是一行 key=value, 便于脚本
 * 直接解析, 也便于人工在 CI 产物里一眼看出结论。 */
static void write_report(const BongoCatApp *app) {
    if (!app || !app->state_root[0]) return;
    char path[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(path, sizeof(path), app->state_root,
            "frame-interval.txt")) return;
    FILE *file = bongo_cat_file_open(path, "wb");
    if (!file) return;
    fprintf(file, "frames=%llu intervals=%zu max_ms=%.1f p99_ms=%.1f p95_ms=%.1f "
        "median_ms=%.1f slowest_at_ns=%llu\n",
        (unsigned long long)probe.total_frames, probe.count,
        (double)probe.slowest_ns / 1000000.0, percentile_ms(0.99),
        percentile_ms(0.95), percentile_ms(0.50),
        (unsigned long long)probe.slowest_at_ns);
    fclose(file);
}

/*
 * 主循环每"真的画了一帧"调一次 (present=true 的那两处 render 之后)。
 * --ci-frame-series 之外零开销直接返回。返回本帧与上一帧的间隔 (纳秒), 第一帧返回 0。
 */
uint64_t bongo_cat_frame_interval_probe(BongoCatApp *app, const char *stage) {
    if (!app || !app->smoke_frame_series) return 0;
    uint64_t now = SDL_GetTicksNS();
    uint64_t gap = probe.last_ns ? now - probe.last_ns : 0;
    probe.last_ns = now;
    probe.total_frames++;
    if (gap) {
        if (probe.count < FRAME_INTERVAL_CAP)
            probe.gaps_ns[probe.count++] = gap;
        if (gap > probe.slowest_ns) {
            probe.slowest_ns = gap;
            probe.slowest_at_ns = now;
            /* 只在大间隔出现时打一行日志 —— 它天然就是"证据行": 时间戳、间隔、当时的
             * 阶段 (渲染发生在模型推进之前还是之后) 一次给全。 */
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[runtime] Frame gap: interval_ms=%.1f stage=%s model=%s "
                "loading=%s switch=%s", (double)gap / 1000000.0,
                stage ? stage : "unknown",
                app->loaded_model[0] ? app->loaded_model : "none",
                app->loading_model[0] ? app->loading_model : "none",
                app->model_switch.active ? app->model_switch.target_id : "none");
        }
    }
    return gap;
}

/* 由主循环与 shutdown 各调一次, 必须是幂等的 (落盘两次写同一份内容, 无害)。
 * 没有采样过就不写文件, 免得 CI 里多出一个空产物。 */
void bongo_cat_frame_interval_report(BongoCatApp *app) {
    if (probe.count) write_report(app);
}
