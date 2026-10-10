#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>

#include "mver_phase_frames.h"

#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/*
 * 相位(时间偏移)分析工具: 给定 Mver 猫和原生猫的抽帧序列, 找出让两段动画最
 * 对齐的帧偏移量, 输出 JSON 供上层脚本挑参数。
 *
 * 四个区域固定是: 整幅 / 脸 / 头发 / 手(见 in_region)。只看全图会被大面积静止
 * 的底色淹没, 所以必须分区看 —— 相位差在不同部位的表现并不一样。
 *
 * 两级搜索: 先用 spatial_stride=3 粗扫 [-max_lag, +max_lag] 的所有偏移, 取运动
 * 相似度最高的前 7 个(再补一个 0)用 stride=1 精算。粗扫只看运动相关性而不看
 * 颜色相似度, 因为颜色相似度对相位几乎不敏感。
 * 输出是 JSON 数组, 字段名是外部脚本读取的契约(见 print_candidate), 不能改。
 */
enum { REGION_COUNT = 4 };

typedef struct Score {
    long long absolute[REGION_COUNT];
    long long samples[REGION_COUNT];
} Score;

typedef struct MotionScore {
    double dot[REGION_COUNT];
    double first_squared[REGION_COUNT];
    double second_squared[REGION_COUNT];
} MotionScore;

typedef struct Candidate {
    int offset;
    double seconds;
    int overlap;
    double similarity[REGION_COUNT];
    double motion[REGION_COUNT];
} Candidate;

/* 区域用归一化比例定义, 因而与分辨率无关; 这些阈值是照着猫的构图量出来的,
   换模型要重新标定。region 0 表示整幅, 直接返回 1。 */
static int in_region(int region, int x, int y, int width, int height) {
    double nx = x / (double)width, ny = y / (double)height;
    if (region == 0) return 1;
    if (region == 1) return nx >= .27 && nx <= .74 && ny >= .24 && ny <= .67;
    if (region == 2) return ny <= .36 || ((nx <= .30 || nx >= .71) && ny <= .70);
    return nx >= .16 && nx <= .58 && ny >= .58 && ny <= .92;
}

/* 逐像素累计颜色差(三通道绝对值之和), 同时记录参与统计的样本数 —— 相似度要
   除以样本数, 而各区域的样本数不同, 不能直接比总和。两边都接近黑(<= 4)的
   像素直接跳过: 那是背景, 加进去只会稀释差异。 */
static void add_pair(Score *score, const Frame *first, const Frame *second,
    int spatial_stride) {
    int gx, gy, region;
    for (gy = 0; gy < first->grid_height; gy += spatial_stride)
    for (gx = 0; gx < first->grid_width; gx += spatial_stride) {
        const unsigned char *a = first->bgr +
            ((size_t)gy * first->grid_width + gx) * 3;
        const unsigned char *b = second->bgr +
            ((size_t)gy * second->grid_width + gx) * 3;
        int first_max = max(a[0], max(a[1], a[2]));
        int second_max = max(b[0], max(b[1], b[2]));
        int absolute, x, y;
        if (first_max <= 4 && second_max <= 4) continue;
        absolute = abs((int)a[0] - b[0]) + abs((int)a[1] - b[1]) +
            abs((int)a[2] - b[2]);
        x = gx * PIXEL_STEP; y = gy * PIXEL_STEP;
        for (region = 0; region < REGION_COUNT; ++region) if (in_region(region,
            x, y, first->width, first->height)) {
            score->absolute[region] += absolute;
            ++score->samples[region];
        }
    }
}

/* 运动相似度用"相邻帧差分向量"的余弦相关: 比较两段动画在每个像素上的变化
   方向是否一致, 因此对整体亮度/配色差异不敏感, 而这正是判断相位所需要的。
   两份差分都为 0 的像素没有方向可言, 跳过; 归一化用 sqrt(f·f * s·s)。 */
static void add_motion_pair(MotionScore *score, const Frame *first_previous,
    const Frame *first, const Frame *second_previous, const Frame *second,
    int spatial_stride) {
    int gx, gy, channel, region;
    for (gy = 0; gy < first->grid_height; gy += spatial_stride)
    for (gx = 0; gx < first->grid_width; gx += spatial_stride) {
        size_t offset = ((size_t)gy * first->grid_width + gx) * 3;
        int x = gx * PIXEL_STEP, y = gy * PIXEL_STEP;
        for (channel = 0; channel < 3; ++channel) {
            double first_delta = first->bgr[offset + channel] -
                first_previous->bgr[offset + channel];
            double second_delta = second->bgr[offset + channel] -
                second_previous->bgr[offset + channel];
            if (first_delta == 0.0 && second_delta == 0.0) continue;
            for (region = 0; region < REGION_COUNT; ++region) if (in_region(
                region, x, y, first->width, first->height)) {
                score->dot[region] += first_delta * second_delta;
                score->first_squared[region] += first_delta * first_delta;
                score->second_squared[region] += second_delta * second_delta;
            }
        }
    }
}

/* 换算成百分比: 每个通道最多差 255, 三通道累计的上限是 samples*3*255。 */
static double similarity(const Score *score, int region) {
    return score->samples[region] == 0 ? 0.0 : 100.0 * (1.0 -
        score->absolute[region] / (score->samples[region] * 3.0 * 255.0));
}

/* 相关系数 [-1,1] 线性映射到 [0,100], 便于和颜色相似度放在一起看。 */
static double motion_similarity(const MotionScore *score, int region) {
    double denominator = sqrt(score->first_squared[region] *
        score->second_squared[region]);
    double correlation;
    if (denominator <= 0.0) return 0.0;
    correlation = score->dot[region] / denominator;
    correlation = max(-1.0, min(1.0, correlation));
    return 50.0 * (correlation + 1.0);
}

/*
 * 算某个偏移量下两段序列的相似度。offset 为正表示原生序列比 Mver 晚 offset 帧:
 * Mver 从 0 开始、原生从 offset 开始, 重叠长度取两者较小值; offset 为负时反过来
 * (mver_start = -offset)。
 * 颜色比较隔帧取样(+= 2)以省时间; 运动比较必须逐帧(+= 1), 否则差分没有意义。
 */
static Candidate measure(const Frame *mver, size_t mver_count,
    const Frame *native, size_t native_count, int offset,
    int interval_milliseconds, int spatial_stride) {
    int mver_start = max(0, -offset), native_start = max(0, offset);
    int overlap = min((int)mver_count - mver_start,
        (int)native_count - native_start);
    Score score = {{0}, {0}};
    MotionScore motion = {{0}, {0}, {0}};
    Candidate candidate;
    int frame, region;
    memset(&candidate, 0, sizeof(candidate));
    for (frame = 0; frame < overlap; frame += 2)
        add_pair(&score, &mver[mver_start + frame],
            &native[native_start + frame], spatial_stride);
    for (frame = 1; frame < overlap; ++frame)
        add_motion_pair(&motion, &mver[mver_start + frame - 1],
            &mver[mver_start + frame], &native[native_start + frame - 1],
            &native[native_start + frame], spatial_stride);
    candidate.offset = offset;
    candidate.seconds = offset * interval_milliseconds / 1000.0;
    candidate.overlap = overlap;
    for (region = 0; region < REGION_COUNT; ++region) {
        candidate.similarity[region] = similarity(&score, region);
        candidate.motion[region] = motion_similarity(&motion, region);
    }
    return candidate;
}

/* 按整幅运动相似度降序; 相同时按偏移升序, 让输出顺序稳定、结果可复现。 */
static int compare_candidates(const void *left, const void *right) {
    const Candidate *a = (const Candidate *)left, *b = (const Candidate *)right;
    if (a->motion[0] < b->motion[0]) return 1;
    if (a->motion[0] > b->motion[0]) return -1;
    return a->offset < b->offset ? -1 : a->offset > b->offset;
}

/* 手工拼 JSON(不引第三方库); %.17g 是为了让 double 能往返可逆。字段名是外部
   脚本读取的契约, 不能改。 */
static void print_candidate(const Candidate *candidate, int comma) {
    printf("  {\"NativeOffsetFrames\":%d,\"NativeOffsetSeconds\":%.17g,"
        "\"OverlapFrames\":%d,\"FullSimilarity\":%.17g,"
        "\"FaceSimilarity\":%.17g,\"HairSimilarity\":%.17g,"
        "\"HandSimilarity\":%.17g,\"FullMotionSimilarity\":%.17g,"
        "\"FaceMotionSimilarity\":%.17g,\"HairMotionSimilarity\":%.17g,"
        "\"HandMotionSimilarity\":%.17g}%s\n", candidate->offset,
        candidate->seconds, candidate->overlap, candidate->similarity[0],
        candidate->similarity[1], candidate->similarity[2],
        candidate->similarity[3], candidate->motion[0], candidate->motion[1],
        candidate->motion[2], candidate->motion[3], comma ? "," : "");
}

/* 参数: MVER_DIR NATIVE_DIR [max-lag-frames=30] [minimum-overlap-frames=2]
   [interval-ms=17]。interval-ms 只用于把帧偏移换算成秒, 默认 17ms 约等于 60fps。 */
int wmain(int argc, wchar_t **argv) {
    PathList mver_paths = {0}, native_paths = {0};
    Frame *mver = NULL, *native = NULL;
    Candidate *coarse = NULL, *fine = NULL;
    size_t coarse_count = 0, fine_count = 0, index;
    int max_lag, minimum_overlap, interval_ms, offset, result = 1;
    int selected_offsets[8], selected_count = 0;
    if (argc < 3 || argc > 6) {
        fwprintf(stderr, L"usage: mver-phase-metrics MVER_DIRECTORY "
            L"NATIVE_DIRECTORY [max-lag-frames] [minimum-overlap-frames] "
            L"[interval-ms]\n");
        return 2;
    }
    /* 强制 C 数字格式: printf 输出的浮点必须用 '.' 作小数点, 否则在某些系统区域
       设置下会写成 ',' , 生成的 JSON 直接失效。 */
    setlocale(LC_NUMERIC, "C");
    max_lag = argc > 3 ? _wtoi(argv[3]) : 30;
    minimum_overlap = argc > 4 ? _wtoi(argv[4]) : 2;
    interval_ms = argc > 5 ? _wtoi(argv[5]) : 17;
    if (max_lag < 0 || minimum_overlap < 2) return 2;
    /* 图像解码走 WIC/GDI+, 需要先初始化 COM; 用 MULTITHREADED 是因为这些工具
       不跑消息循环。 */
    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) return 1;
    if (!phase_collect_paths(argv[1], &mver_paths) ||
        !phase_collect_paths(argv[2], &native_paths) ||
        mver_paths.count < (size_t)minimum_overlap ||
        native_paths.count < (size_t)minimum_overlap) goto done;
    mver = phase_load_frames(&mver_paths);
    native = phase_load_frames(&native_paths);
    if (!mver || !native) goto done;
    coarse = (Candidate *)malloc((size_t)(max_lag * 2 + 1) * sizeof(*coarse));
    fine = (Candidate *)malloc(8 * sizeof(*fine));
    if (!coarse || !fine) goto done;
    for (offset = -max_lag; offset <= max_lag; ++offset) {
        Candidate candidate = measure(mver, mver_paths.count, native,
            native_paths.count, offset, interval_ms, 3);
        if (candidate.overlap >= minimum_overlap) coarse[coarse_count++] = candidate;
    }
    if (!coarse_count) goto done;
    qsort(coarse, coarse_count, sizeof(*coarse), compare_candidates);
    for (index = 0; index < min((size_t)7, coarse_count); ++index)
        selected_offsets[selected_count++] = coarse[index].offset;
    {
        int found_zero = 0;
        for (index = 0; index < (size_t)selected_count; ++index)
            if (selected_offsets[index] == 0) found_zero = 1;
        if (!found_zero) selected_offsets[selected_count++] = 0;
    }
    for (index = 0; index < (size_t)selected_count; ++index) {
        Candidate candidate = measure(mver, mver_paths.count, native,
            native_paths.count, selected_offsets[index], interval_ms, 1);
        if (candidate.overlap >= minimum_overlap) fine[fine_count++] = candidate;
    }
    qsort(fine, fine_count, sizeof(*fine), compare_candidates);
    printf("[\n");
    for (index = 0; index < fine_count; ++index)
        print_candidate(&fine[index], index + 1 < fine_count);
    printf("]\n");
    result = 0;
done:
    free(coarse); free(fine);
    phase_free_frames(mver, mver_paths.count);
    phase_free_frames(native, native_paths.count);
    phase_free_paths(&mver_paths); phase_free_paths(&native_paths);
    CoUninitialize();
    if (result) fwprintf(stderr, L"phase analysis failed\n");
    return result;
}
