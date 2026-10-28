/*
 * 自检的编排、总结与报告落盘 (分组实现见 selfcheck_env/render/live2d/files.c)。
 *
 * 这里的顺序就是面板与报告里的顺序, 也是"从外到内"的排查顺序:
 *   版本与系统 -> 渲染后端 -> Live2D -> 文件与权限 -> 配置 -> 音频 -> 输入 -> 总结
 * 前两组回答"这台机器/这次构建能不能把一帧画出来"; 中间两组回答"需要的文件在不在、
 * 能不能读写"; 后三组是桌宠能不能响应我的补充证据。
 *
 * 两条纪律:
 *  1. 每项都必须有结论。拿不到数据不是"未知", 而是按失败/警告写清拿不到的原因 ——
 *     用户拿这份报告去求助时, 一句"未知"没有任何信息量;
 *  2. 结论必须能独立读懂: 带路径、版本、原因码, 不假设读者见过日志。
 */
#include "selfcheck_internal.h"

#include "bongo_cat/app.h"
#include "bongo_cat/file.h"
#include "bongo_cat/log.h"
#include "bongo_cat/path.h"
#include "storage_paths.h"

#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define BONGO_CAT_SELFCHECK_SUMMARY_SECTION "总结"

static const char *status_text(BongoCatSelfCheckStatus status) {
    switch (status) {
    case BONGO_CAT_SELFCHECK_PASS: return "通过";
    case BONGO_CAT_SELFCHECK_WARN: return "警告";
    case BONGO_CAT_SELFCHECK_FAIL: return "失败";
    default: return "失败";
    }
}

void bongo_cat_selfcheck_section(BongoCatSelfCheckBuilder *builder,
    const char *title) {
    if (!builder) return;
    snprintf(builder->section, sizeof(builder->section), "%s",
        title ? title : "");
}

void bongo_cat_selfcheck_add(BongoCatSelfCheckBuilder *builder,
    BongoCatSelfCheckStatus status, const char *title, const char *format, ...) {
    if (!builder || !builder->report) return;
    BongoCatSelfCheckReport *report = builder->report;
    if (report->count >= BONGO_CAT_SELFCHECK_ITEM_CAP) return;
    BongoCatSelfCheckItem *item = &report->items[report->count++];
    snprintf(item->section, sizeof(item->section), "%s", builder->section);
    snprintf(item->title, sizeof(item->title), "%s", title ? title : "");
    item->status = status;
    va_list args;
    va_start(args, format);
    vsnprintf(item->detail, sizeof(item->detail), format ? format : "", args);
    va_end(args);
}

static void timestamp_text(char *output, size_t capacity, bool file_name) {
    SDL_Time ticks = 0;
    SDL_DateTime date = {0};
    if (!SDL_GetCurrentTime(&ticks) ||
        !SDL_TimeToDateTime(ticks, &date, true)) {
        snprintf(output, capacity, "%s", file_name ? "unknown-time" : "未知时间");
        return;
    }
    if (file_name)
        snprintf(output, capacity, "%04d%02d%02d-%02d%02d%02d", date.year,
            date.month, date.day, date.hour, date.minute, date.second);
    else
        snprintf(output, capacity, "%04d-%02d-%02d %02d:%02d:%02d", date.year,
            date.month, date.day, date.hour, date.minute, date.second);
}

/* 统计只看真正的检查项: 总结那一条不参与计数, 否则"全部通过"会被自己算成 1 项 */
static void recount(BongoCatSelfCheckReport *report) {
    report->pass = report->warn = report->fail = 0;
    for (size_t i = 0; i < report->count; ++i) {
        if (strcmp(report->items[i].section,
                BONGO_CAT_SELFCHECK_SUMMARY_SECTION) == 0) continue;
        switch (report->items[i].status) {
        case BONGO_CAT_SELFCHECK_PASS: report->pass++; break;
        case BONGO_CAT_SELFCHECK_WARN: report->warn++; break;
        default: report->fail++; break;
        }
    }
}

/*
 * 总结。两条硬语义:
 *   - 一句"全部通过"必须是真的全部通过 (警告也算不通过, 但要分开数);
 *   - 发现 Vulkan 首帧失败时, 结论必须直说"切换渲染后端为 OpenGL" —— 这是用户能
 *     立刻自救的唯一动作, 藏在别的文字里等于没说。
 */
static void compose_summary(BongoCatSelfCheckReport *report,
    BongoCatSelfCheckBuilder *builder) {
    if (report->fail)
        snprintf(report->summary, sizeof(report->summary),
            "自检发现 %u 项失败、%u 项警告、%u 项通过。%s",
            report->fail, report->warn, report->pass,
            report->recommend_opengl
                ? "Vulkan 首帧呈现失败，建议切换渲染后端为 OpenGL。"
                : "请按失败的条目逐项处理。");
    else if (report->warn)
        snprintf(report->summary, sizeof(report->summary),
            "自检通过，但有 %u 项警告（%u 项通过）。%s", report->warn,
            report->pass,
            report->recommend_opengl
                ? "建议切换渲染后端为 OpenGL。"
                : "警告项不影响启动，可对照说明确认。");
    else
        snprintf(report->summary, sizeof(report->summary),
            "全部 %u 项通过，运行环境正常。", report->pass);
    BongoCatSelfCheckStatus status = report->fail ? BONGO_CAT_SELFCHECK_FAIL :
        (report->warn ? BONGO_CAT_SELFCHECK_WARN : BONGO_CAT_SELFCHECK_PASS);
    bongo_cat_selfcheck_section(builder, BONGO_CAT_SELFCHECK_SUMMARY_SECTION);
    bongo_cat_selfcheck_add(builder, status, "总结", "%s", report->summary);
}

/* 把条目拼成一份可以整段粘贴的报告 */
static void append_text(char *text, size_t capacity, int *used,
    const char *format, ...) {
    if (!text || !used || *used < 0 || (size_t)*used >= capacity) return;
    va_list args;
    va_start(args, format);
    int written = vsnprintf(text + *used, capacity - (size_t)*used, format, args);
    va_end(args);
    if (written < 0) { text[*used] = '\0'; return; }
    *used += written;
    /* snprintf 返回"本应写入的长度": 截断之后 used 会超过容量, 收敛到末尾,
       后续 append 直接放弃 (报告是给人看的, 截断比溢出更重要)。 */
    if ((size_t)*used >= capacity) *used = (int)capacity - 1;
}

static void compose_text(BongoCatSelfCheckReport *report) {
    char now[32];
    timestamp_text(now, sizeof(now), false);
    int used = 0;
    append_text(report->text, sizeof(report->text), &used,
        "%s %s 自检报告\n时间: %s\n耗时: %u ms\n"
        "结果: %u 项通过 / %u 项警告 / %u 项失败\n",
        BONGO_CAT_NAME, BONGO_CAT_VERSION, now, report->elapsed_ms,
        report->pass, report->warn, report->fail);
    for (size_t i = 0; i < report->count; ++i) {
        const BongoCatSelfCheckItem *item = &report->items[i];
        if (i == 0 || strcmp(report->items[i - 1].section, item->section) != 0)
            append_text(report->text, sizeof(report->text), &used,
                "\n-- %s --\n", item->section);
        append_text(report->text, sizeof(report->text), &used,
            "[%s] %s: %s\n", status_text(item->status), item->title,
            item->detail);
    }
    append_text(report->text, sizeof(report->text), &used,
        "\n本报告由 BongoCat 自检生成, 不包含任何配置或模型改动。\n");
}

void bongo_cat_selfcheck_run(struct BongoCatApp *app,
    BongoCatSelfCheckReport *report) {
    if (!report) return;
    memset(report, 0, sizeof(*report));
    const uint64_t started = SDL_GetTicksNS();
    BongoCatSelfCheckBuilder builder = {.report = report};
    bongo_cat_selfcheck_env(&builder, app);
    bongo_cat_selfcheck_render(&builder, app);
    bongo_cat_selfcheck_live2d(&builder, app);
    bongo_cat_selfcheck_files(&builder, app);
    bongo_cat_selfcheck_config(&builder, app);
    bongo_cat_selfcheck_audio(&builder, app);
    bongo_cat_selfcheck_input(&builder, app);
    report->elapsed_ms =
        (unsigned)((SDL_GetTicksNS() - started) / 1000000ull);
    recount(report);
    compose_summary(report, &builder);
    compose_text(report);
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "[selfcheck] 自检完成: 通过=%u 警告=%u 失败=%u 耗时=%ums 建议OpenGL=%d",
        report->pass, report->warn, report->fail, report->elapsed_ms,
        report->recommend_opengl ? 1 : 0);
    /*
     * 非通过项逐条进日志。用户回传的往往只有日志 (报告面板是截图), 而"哪一项失败、
     * 为什么"正是支持最需要的第一手信息 —— 只写一个汇总计数等于让人再问一轮。
     */
    for (size_t i = 0; i < report->count; ++i) {
        const BongoCatSelfCheckItem *item = &report->items[i];
        if (item->status == BONGO_CAT_SELFCHECK_PASS) continue;
        if (item->status == BONGO_CAT_SELFCHECK_WARN)
            SDL_LogWarn(BONGO_CAT_LOG_LIFECYCLE, "[selfcheck] 警告 %s: %s",
                item->title, item->detail);
        else
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "[selfcheck] 失败 %s: %s", item->title, item->detail);
    }
}

bool bongo_cat_selfcheck_default_path(char *output, size_t capacity) {
    if (!output || capacity == 0) return false;
    output[0] = '\0';
    char root[BONGO_CAT_PATH_CAP], directory[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_storage_paths_root(root, sizeof(root)) || !root[0])
        return false;
    if (!bongo_cat_path_join(directory, sizeof(directory), root, "logs") ||
        !bongo_cat_path_create_directory(directory))
        return false;
    char stamp[32];
    timestamp_text(stamp, sizeof(stamp), true);
    char name[96];
    snprintf(name, sizeof(name), "selfcheck-%s.txt", stamp);
    return bongo_cat_path_join(output, capacity, directory, name);
}

bool bongo_cat_selfcheck_save(const BongoCatSelfCheckReport *report,
    const char *path, BongoCatError *error) {
    if (!report || !path || !path[0]) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_ARGUMENT,
            "自检报告没有可写入的路径");
        return false;
    }
    FILE *file = bongo_cat_file_open(path, "wb");
    if (!file) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "无法写入自检报告: %s", path);
        return false;
    }
    size_t length = strlen(report->text);
    bool written = length > 0 && fwrite(report->text, 1, length, file) == length;
    if (fclose(file) != 0) written = false;
    if (!written) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_IO,
            "自检报告写入失败 (磁盘已满或无权限): %s", path);
        return false;
    }
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE, "[selfcheck] 报告已保存: %s", path);
    return true;
}
