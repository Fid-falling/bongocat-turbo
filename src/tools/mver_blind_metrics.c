#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>

#include "validation_image.h"

#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int maximum_rgb(const unsigned char *pixel) {
    return max(pixel[0], max(pixel[1], pixel[2]));
}

int wmain(int argc, wchar_t **argv) {
    BongoCatValidationImage first = {0}, second = {0}, mask = {0};
    int sample_step, alpha_threshold, background_threshold, step, alpha_step;
    int alpha_samples = 0, non_opaque = 0, alpha_varies;
    long long within = 0, samples = 0, foreground_within = 0, foreground = 0;
    long long foreground_union = 0, foreground_intersection = 0;
    double absolute = 0.0, foreground_absolute = 0.0;
    int x, y, result = 1, have_mask;
    if (argc < 3 || argc > 7) {
        fwprintf(stderr, L"usage: mver-blind-metrics LEFT RIGHT [mask] "
            L"[sample-step] [alpha-threshold] [background-threshold]\n");
        return 2;
    }
    setlocale(LC_NUMERIC, "C");
    sample_step = argc > 4 ? _wtoi(argv[4]) : 0;
    alpha_threshold = argc > 5 ? _wtoi(argv[5]) : 8;
    background_threshold = argc > 6 ? _wtoi(argv[6]) : 4;
    have_mask = argc > 3 && argv[3][0] != L'\0' && wcscmp(argv[3], L"-") != 0;
    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) return 1;
    if (!bongo_cat_validation_image_load(argv[1], &first) ||
        !bongo_cat_validation_image_load(argv[2], &second) ||
        (have_mask && !bongo_cat_validation_image_load(argv[3], &mask))) goto done;
    if (first.width != second.width || first.height != second.height) {
        fwprintf(stderr, L"Frame dimensions differ\n"); goto done;
    }
    step = sample_step > 0 ? sample_step : max(1, (int)sqrt(
        first.width * (double)first.height / 250000.0));
    alpha_step = max(step, 8);
    for (y = 0; y < first.height; y += alpha_step)
    for (x = 0; x < first.width; x += alpha_step) {
        const unsigned char *a = first.bgra + ((size_t)y * first.width + x) * 4;
        const unsigned char *b = second.bgra + ((size_t)y * second.width + x) * 4;
        if (a[3] < 255 || b[3] < 255) ++non_opaque;
        ++alpha_samples;
    }
    alpha_varies = non_opaque >= max(2, alpha_samples / 100);
    for (y = 0; y < first.height; y += step)
    for (x = 0; x < first.width; x += step) {
        const unsigned char *a = first.bgra + ((size_t)y * first.width + x) * 4;
        const unsigned char *b = second.bgra + ((size_t)y * second.width + x) * 4;
        int maximum = 0, foreground_maximum = 0, channel;
        int first_visible, second_visible, is_foreground;
        for (channel = 0; channel < 4; ++channel) {
            int delta = abs((int)a[channel] - b[channel]);
            absolute += delta; maximum = max(maximum, delta);
        }
        if (maximum <= 8) ++within;
        first_visible = alpha_varies ? a[3] > alpha_threshold :
            maximum_rgb(a) > background_threshold;
        second_visible = alpha_varies ? b[3] > alpha_threshold :
            maximum_rgb(b) > background_threshold;
        if (first_visible || second_visible) ++foreground_union;
        if (first_visible && second_visible) ++foreground_intersection;
        is_foreground = first_visible || second_visible;
        if (have_mask) {
            int mask_x = min(mask.width - 1,
                (int)(x * (double)mask.width / first.width));
            int mask_y = min(mask.height - 1,
                (int)(y * (double)mask.height / first.height));
            const unsigned char *m = mask.bgra +
                ((size_t)mask_y * mask.width + mask_x) * 4;
            is_foreground = m[3] > alpha_threshold ||
                maximum_rgb(m) > alpha_threshold;
        }
        if (is_foreground) {
            for (channel = 0; channel < 4; ++channel) {
                int delta = abs((int)a[channel] - b[channel]);
                foreground_absolute += delta;
                foreground_maximum = max(foreground_maximum, delta);
            }
            if (foreground_maximum <= 8) ++foreground_within;
            ++foreground;
        }
        ++samples;
    }
    if (foreground == 0) {
        fwprintf(stderr, L"No foreground pixels found\n"); goto done;
    }
    printf("{\n"
        "  \"Width\": %d,\n"
        "  \"Height\": %d,\n"
        "  \"Similarity\": %.17g,\n"
        "  \"WithinEight\": %.17g,\n"
        "  \"ForegroundSimilarity\": %.17g,\n"
        "  \"ForegroundWithinEight\": %.17g,\n"
        "  \"ForegroundSamples\": %lld,\n"
        "  \"ForegroundMode\": \"%s\",\n"
        "  \"ForegroundIoU\": %.17g\n"
        "}\n", first.width, first.height,
        100.0 * (1.0 - absolute / (samples * 4.0 * 255.0)),
        100.0 * within / samples,
        100.0 * (1.0 - foreground_absolute / (foreground * 4.0 * 255.0)),
        100.0 * foreground_within / foreground, foreground,
        have_mask ? "mask" : alpha_varies ? "alpha-union" :
            "black-background-union",
        foreground_union ? 100.0 * foreground_intersection / foreground_union : 100.0);
    result = 0;
done:
    bongo_cat_validation_image_free(&first);
    bongo_cat_validation_image_free(&second);
    bongo_cat_validation_image_free(&mask);
    CoUninitialize();
    return result;
}
