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

static int in_region(int region, int x, int y, int width, int height) {
    double nx = x / (double)width, ny = y / (double)height;
    if (region == 0) return 1;
    if (region == 1) return nx >= .27 && nx <= .74 && ny >= .24 && ny <= .67;
    if (region == 2) return ny <= .36 || ((nx <= .30 || nx >= .71) && ny <= .70);
    return nx >= .16 && nx <= .58 && ny >= .58 && ny <= .92;
}

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

static double similarity(const Score *score, int region) {
    return score->samples[region] == 0 ? 0.0 : 100.0 * (1.0 -
        score->absolute[region] / (score->samples[region] * 3.0 * 255.0));
}

static double motion_similarity(const MotionScore *score, int region) {
    double denominator = sqrt(score->first_squared[region] *
        score->second_squared[region]);
    double correlation;
    if (denominator <= 0.0) return 0.0;
    correlation = score->dot[region] / denominator;
    correlation = max(-1.0, min(1.0, correlation));
    return 50.0 * (correlation + 1.0);
}

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

static int compare_candidates(const void *left, const void *right) {
    const Candidate *a = (const Candidate *)left, *b = (const Candidate *)right;
    if (a->motion[0] < b->motion[0]) return 1;
    if (a->motion[0] > b->motion[0]) return -1;
    return a->offset < b->offset ? -1 : a->offset > b->offset;
}

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
    setlocale(LC_NUMERIC, "C");
    max_lag = argc > 3 ? _wtoi(argv[3]) : 30;
    minimum_overlap = argc > 4 ? _wtoi(argv[4]) : 2;
    interval_ms = argc > 5 ? _wtoi(argv[5]) : 17;
    if (max_lag < 0 || minimum_overlap < 2) return 2;
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
