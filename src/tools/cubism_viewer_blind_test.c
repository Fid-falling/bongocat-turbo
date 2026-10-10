#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>

#include "cubism_viewer_blind_frames.h"
#include "validation_image.h"
#include "windows_tool.h"

#include <limits.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/*
 * 盲测生成器: 把 viewer 与原生实现渲染的同一场景归一化, 生成随机左右顺序的对照图
 * (ballots)与答案键(answer-key.csv), 同时算出一组客观指标(metrics.csv)。
 *
 * 为什么要盲测: 肉眼看"哪个更像"很容易被先入为主影响, 所以左右顺序由固定种子的
 * 伪随机数决定, 看图的人不知道哪边是哪边, 答案单独写另一个文件。
 * 种子必须可复现: DotNetRandom 复刻的是 .NET System.Random 的算法(56 元种子数组
 * 加 Knuth 减法发生器), 这样 .NET 侧能重建出完全相同的左右顺序。
 * CANVAS / MARGIN 必须与 validation_gdiplus.c 里归一化用的常量一致。
 */
enum { CANVAS = 800, MARGIN = 40 };

typedef struct Region {
    const char *name;
    int left, top, right, bottom;
} Region;

typedef struct DotNetRandom {
    int seed[56];
    int inext;
    int inextp;
} DotNetRandom;

/* 与 validation_image.c 里的同名函数保持同一个阈值(237): 两处判前景的口径必须
   一致, 否则"保留最大连通块"和"统计前景像素"会得出互相矛盾的结论。 */
static int foreground_pixel(const unsigned char *pixel) {
    return min(pixel[0], min(pixel[1], pixel[2])) < 237;
}

/*
 * 写一行指标: 相似度 / 8 以内像素占比 / 前景 IoU / viewer 变化量 / native 变化量 /
 * 响应比。
 * 几个约定: 采样步长固定为 2(区域是 800 画布上的像素框, 全采样太慢); 只统计
 * 至少一侧是前景的像素(背景占绝大多数, 算进去会把差异摊平); viewer_change 与
 * native_change 是相对同一基线帧(通常 track-000)的变化量, 而 response_ratio 是
 * 两者之比 —— 它回答"原生实现的形变幅度与 viewer 是否相当", 比单纯比像素更有意义。
 * viewer 变化量为 0 时用 999 而不是无穷大, 保证 CSV 仍然可被解析。
 */
static int write_metrics(FILE *writer, const Frame *frame, const Region *region,
    const BongoCatValidationImage *viewer, const BongoCatValidationImage *native,
    const BongoCatValidationImage *viewer_base,
    const BongoCatValidationImage *native_base) {
    long long count = 0, within = 0, intersection = 0, union_count = 0;
    double absolute = 0, viewer_change = 0, native_change = 0;
    int x, y;
    for (y = region->top; y < region->bottom; y += 2)
    for (x = region->left; x < region->right; x += 2) {
        const unsigned char *a = viewer->bgra + ((size_t)y * viewer->width + x) * 4;
        const unsigned char *b = native->bgra + ((size_t)y * native->width + x) * 4;
        const unsigned char *av = viewer_base->bgra +
            ((size_t)y * viewer_base->width + x) * 4;
        const unsigned char *bn = native_base->bgra +
            ((size_t)y * native_base->width + x) * 4;
        int first = foreground_pixel(a), second = foreground_pixel(b);
        int maximum = 0, channel;
        if (first || second) ++union_count;
        if (first && second) ++intersection;
        if (!first && !second) continue;
        for (channel = 0; channel < 3; ++channel) {
            int delta = abs((int)a[channel] - b[channel]);
            maximum = max(maximum, delta); absolute += delta;
            viewer_change += abs((int)a[channel] - av[channel]);
            native_change += abs((int)b[channel] - bn[channel]);
        }
        if (maximum <= 8) ++within;
        ++count;
    }
    {
        double similarity = count == 0 ? 100.0 : 100.0 *
            (1.0 - absolute / (count * 3.0 * 255.0));
        double near_value = count == 0 ? 100.0 : 100.0 * within / count;
        double iou = union_count == 0 ? 100.0 : 100.0 * intersection / union_count;
        double viewer_ratio = count == 0 ? 0.0 : viewer_change /
            (count * 3.0 * 255.0);
        double native_ratio = count == 0 ? 0.0 : native_change /
            (count * 3.0 * 255.0);
        double ratio = viewer_ratio < 0.000001 ?
            (native_ratio < 0.000001 ? 1.0 : 999.0) : native_ratio / viewer_ratio;
        return fprintf(writer, "%ls,%s,%.4f,%.4f,%.4f,%.6f,%.6f,%.4f,%lld\n",
            frame->name, region->name, similarity, near_value, iou, viewer_ratio,
            native_ratio, ratio, count) >= 0;
    }
}

/* 拼一张左右并排的对照图(底色 238 灰, 上方留 60 像素写 "A"/"B")并存成 PNG。
   viewer_first 决定哪边是 A, 这个布尔来自随机数, 调用方同时把答案写进 answer-key。 */
static int write_ballot(const wchar_t *path, const Frame *frame, int viewer_first) {
    BongoCatValidationImage ballot = {0};
    const BongoCatValidationImage *first = viewer_first ? &frame->viewer : &frame->native;
    const BongoCatValidationImage *second = viewer_first ? &frame->native : &frame->viewer;
    int y, x;
    ballot.width = CANVAS * 2 + 30; ballot.height = CANVAS + 60;
    ballot.bgra = (unsigned char *)malloc((size_t)ballot.width * ballot.height * 4);
    if (!ballot.bgra) return 0;
    for (y = 0; y < ballot.height; ++y) for (x = 0; x < ballot.width; ++x) {
        unsigned char *pixel = ballot.bgra + ((size_t)y * ballot.width + x) * 4;
        pixel[0] = pixel[1] = pixel[2] = 238; pixel[3] = 255;
    }
    for (y = 0; y < CANVAS; ++y) {
        memcpy(ballot.bgra + ((size_t)(y + 60) * ballot.width) * 4,
            first->bgra + (size_t)y * CANVAS * 4, CANVAS * 4);
        memcpy(ballot.bgra + ((size_t)(y + 60) * ballot.width + CANVAS + 30) * 4,
            second->bgra + (size_t)y * CANVAS * 4, CANVAS * 4);
    }
    if (!bongo_cat_validation_image_draw_label(&ballot, L"A",
            CANVAS / 2.0f - 10.0f, 15.0f) ||
        !bongo_cat_validation_image_draw_label(&ballot, L"B",
            CANVAS + 30.0f + CANVAS / 2.0f - 10.0f, 15.0f)) {
        bongo_cat_validation_image_free(&ballot); return 0;
    }
    x = bongo_cat_validation_image_save(path, &ballot, 1);
    bongo_cat_validation_image_free(&ballot);
    return x;
}

/* .NET System.Random 的初始化(56 元种子数组 + Knuth 减法发生器), 逐行照抄是为了
   与 .NET 侧产生完全相同的随机序列; 这里的 INT_MAX 扮演 .NET 里的 MBIG。
   不要"优化"这些表达式, 它们就是要和原实现一模一样。 */
static void random_init(DotNetRandom *random, int seed) {
    int subtraction = seed == INT_MIN ? INT_MAX : abs(seed);
    int mj = 161803398 - subtraction, mk = 1, i, ii, k;
    memset(random, 0, sizeof(*random));
    random->seed[55] = mj;
    for (i = 1; i < 55; ++i) {
        ii = (21 * i) % 55; random->seed[ii] = mk;
        mk = mj - mk; if (mk < 0) mk += INT_MAX; mj = random->seed[ii];
    }
    for (k = 1; k < 5; ++k) for (i = 1; i < 56; ++i) {
        int n = 1 + (i + 30) % 55;
        random->seed[i] -= random->seed[n];
        if (random->seed[i] < 0) random->seed[i] += INT_MAX;
    }
    random->inext = 0; random->inextp = 21;
}

/* 返回 0 或 1(把 [0, INT_MAX) 映射到两个桶), 用来决定对照图的左右顺序。 */
static int random_next_two(DotNetRandom *random) {
    int loc, locp, value;
    loc = random->inext + 1; if (loc >= 56) loc = 1; random->inext = loc;
    locp = random->inextp + 1; if (locp >= 56) locp = 1; random->inextp = locp;
    value = random->seed[loc] - random->seed[locp];
    if (value == INT_MAX) --value;
    if (value < 0) value += INT_MAX;
    random->seed[loc] = value;
    return (int)((value / (double)INT_MAX) * 2.0);
}

/* 记下本次使用的种子与帧数: 复核时只要种子相同, 就能重建同样的左右顺序。 */
static int write_summary(const wchar_t *path, int seed, size_t frames) {
    FILE *file = _wfopen(path, L"wb");
    if (!file) return 0;
    fprintf(file, "{\n  \"seed\": %d,\n  \"frames\": %zu,\n  \"canvas\": 800\n}\n",
        seed, frames);
    return fclose(file) == 0;
}

/* 参数: viewer 目录 native 目录 输出目录 [seed]。seed 为 0 时用当前 tick 现取。 */
int wmain(int argc, wchar_t **argv) {
    FrameList frames = {0};
    wchar_t *normalized = NULL, *ballots = NULL, *metrics_path = NULL;
    wchar_t *key_path = NULL, *summary_path = NULL;
    BongoCatValidationRect viewer_bounds, native_bounds;
    size_t baseline_index = 0, index;
    int seed, result = 1;
    /* 四个统计区域(800x800 归一化画布上的像素框): 整幅 / 脸 / 头发 / 手。
       与 mver_phase_metrics.c 的分区思路一致 —— 不同部位的表现差异很大。 */
    const Region regions[] = {
        {"full", 0, 0, CANVAS, CANVAS},
        {"face", 225, 170, 575, 455},
        {"hair", 70, 40, 730, 495},
        {"hands", 55, 395, 745, 710}
    };
    setlocale(LC_NUMERIC, "C");
    if (argc < 4) {
        fwprintf(stderr, L"usage: blind-test.exe viewer native output [seed]\n");
        return 2;
    }
    seed = argc > 4 ? _wtoi(argv[4]) : 20260807;
    if (seed == 0) seed = (int)(GetTickCount() & INT_MAX);
    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) return 1;
    if (!bongo_cat_tool_ensure_directory(argv[3]) ||
        !blind_collect_frames(argv[1], argv[2], &frames))
        goto done;
    normalized = blind_join_path(argv[3], L"normalized");
    ballots = blind_join_path(argv[3], L"ballots");
    metrics_path = blind_join_path(argv[3], L"metrics.csv");
    key_path = blind_join_path(argv[3], L"answer-key.csv");
    summary_path = blind_join_path(argv[3], L"summary.json");
    if (!normalized || !ballots || !metrics_path || !key_path || !summary_path ||
        !bongo_cat_tool_ensure_directory(normalized) ||
        !bongo_cat_tool_ensure_directory(ballots)) goto done;
    for (index = 0; index < frames.count; ++index)
        /* track-000 = 刚按下、还没移动的那一帧, 用它当"变化量"的基线: 其余帧都
           相对它来计算形变幅度。 */
        if (wcscmp(frames.items[index].name, L"track-000") == 0) baseline_index = index;
    {
        /* 包围盒基准优先取 idle 帧(角色处于静止的标准姿势), 没有则退回 track-000。
           viewer 与 native 各算各的包围盒 —— 两个实现的位置和尺寸本来就有差异,
           归一化正是要把这点差异消掉。 */
        wchar_t *idle = blind_join_suffix(argv[1], L"idle", L".png");
        int have_idle = idle && _waccess(idle, 0) == 0;
        wchar_t *viewer_bounds_path = have_idle ? idle : frames.items[baseline_index].viewer_path;
        BongoCatValidationImage bounds_image = {0};
        if (!bongo_cat_validation_image_load(viewer_bounds_path, &bounds_image) ||
            !bongo_cat_validation_image_bounds(&bounds_image, &viewer_bounds)) {
            bongo_cat_validation_image_free(&bounds_image); free(idle); goto done;
        }
        bongo_cat_validation_image_free(&bounds_image);
        if (!bongo_cat_validation_image_load(frames.items[baseline_index].native_path,
                &bounds_image) || !bongo_cat_validation_image_bounds(&bounds_image,
                &native_bounds)) {
            bongo_cat_validation_image_free(&bounds_image); free(idle); goto done;
        }
        bongo_cat_validation_image_free(&bounds_image); free(idle);
    }
    for (index = 0; index < frames.count; ++index) {
        wchar_t *viewer_output, *native_output;
        if (!bongo_cat_validation_image_normalize_file(
                frames.items[index].viewer_path, viewer_bounds,
                &frames.items[index].viewer) ||
            !bongo_cat_validation_image_normalize_file(
                frames.items[index].native_path, native_bounds,
                &frames.items[index].native) ||
            !bongo_cat_validation_image_keep_largest(&frames.items[index].viewer) ||
            !bongo_cat_validation_image_keep_largest(&frames.items[index].native)) {
            goto done;
        }
        viewer_output = blind_join_suffix(normalized, frames.items[index].name, L"-viewer.png");
        native_output = blind_join_suffix(normalized, frames.items[index].name, L"-native.png");
        if (!viewer_output || !native_output ||
            !bongo_cat_validation_image_save(viewer_output, &frames.items[index].viewer, 1) ||
            !bongo_cat_validation_image_save(native_output, &frames.items[index].native, 1)) {
            free(viewer_output); free(native_output); goto done;
        }
        free(viewer_output); free(native_output);
    }
    {
        /* 指标与答案键必须成对写: 每一帧先写四个区域的指标, 再生成一张对照图并把
           左右顺序追加到 answer-key, 因此两张表的行顺序是同步的。 */
        FILE *metrics = _wfopen(metrics_path, L"w");
        FILE *key = _wfopen(key_path, L"w");
        DotNetRandom random;
        if (!metrics || !key) { if (metrics) fclose(metrics); if (key) fclose(key); goto done; }
        fprintf(metrics, "frame,region,similarity,within8,foreground_iou,viewer_change,native_change,response_ratio,samples\n");
        fprintf(key, "frame,A,B\n");
        random_init(&random, seed);
        for (index = 0; index < frames.count; ++index) {
            size_t region;
            for (region = 0; region < sizeof(regions) / sizeof(regions[0]); ++region)
                if (!write_metrics(metrics, &frames.items[index], &regions[region],
                    &frames.items[index].viewer, &frames.items[index].native,
                    &frames.items[baseline_index].viewer, &frames.items[baseline_index].native)) {
                    fclose(metrics); fclose(key); goto done;
                }
            {
                int viewer_first = random_next_two(&random) == 0;
                wchar_t *ballot = blind_join_suffix(ballots, frames.items[index].name, L".png");
                if (!ballot || !write_ballot(ballot, &frames.items[index], viewer_first)) {
                    free(ballot); fclose(metrics); fclose(key); goto done;
                }
                free(ballot);
                fprintf(key, "%ls,%s\n", frames.items[index].name,
                    viewer_first ? "Viewer,Native" : "Native,Viewer");
            }
        }
        fclose(metrics); fclose(key);
    }
    if (!write_summary(summary_path, seed, frames.count)) goto done;
    wprintf(L"%ls\n", argv[3]);
    result = 0;
done:
    free(normalized); free(ballots); free(metrics_path); free(key_path); free(summary_path);
    blind_free_frames(&frames);
    CoUninitialize();
    if (result) fwprintf(stderr, L"blind test failed\n");
    return result;
}
