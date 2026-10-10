/* 读取 --ci-viewer-trace= 指向的参考 trace, 把抓图时刻换算回帧号: 6 个 track 与 6 个 return 名字各取
 * 一个, 用来验证这一轮的抓帧时机与参考运行一致 —— 时机漂了, 回归数据就不再可比。没给参数或文件不存在
 * 时返回 true (首次运行没有基线), 缺行或顺序倒退返回 false。track/returning 由调用方提供, 各需 6 个 int。
 */
#include "live2d_viewer_timing.h"
#include "bongo_cat/file.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 空路径与打不开的路径语义不同: 空 = 不需要检查 (true), 有路径但 open 失败 = 失败。
 * 每行至少要能解析出三列 (名字、一个被忽略的数值、毫秒时刻), 毫秒换算成帧号后会夹到 >=1,
 * 因为第 0 帧无法作为抓图时机; 名字不在那 12 个之内的行直接忽略, 允许 trace 带额外条目。
 */
bool bongo_cat_live2d_viewer_timing(const char *path, int *track,
    int *returning) {
    static const char *track_names[] = {"track-001", "track-002",
        "track-004", "track-008", "track-015", "track-030"};
    static const char *return_names[] = {"return-001", "return-002",
        "return-004", "return-008", "return-015", "return-030"};
    if (!path || !path[0]) return true;
    FILE *file = bongo_cat_file_open(path, "rb");
    if (!file) return false;
    bool found_track[6] = {0}, found_return[6] = {0};
    char line[512], name[32];
    while (fgets(line, sizeof(line), file)) {
        char *first = strchr(line, ',');
        size_t name_length = first ? (size_t)(first - line) : 0;
        if (!first || !name_length || name_length >= sizeof(name)) continue;
        memcpy(name, line, name_length); name[name_length] = '\0';
        char *after_phase = NULL, *after_mid = NULL;
        (void)strtod(first + 1, &after_phase);
        if (after_phase == first + 1 || *after_phase != ',') continue;
        double capture_mid_ms = strtod(after_phase + 1, &after_mid);
        if (after_mid == after_phase + 1) continue;
        int frame = (int)lround(capture_mid_ms * 60.0 / 1000.0);
        if (frame < 1) frame = 1;
        for (size_t i = 0; i < 6; ++i) {
            if (strcmp(name, track_names[i]) == 0) {
                track[i] = frame; found_track[i] = true;
            } else if (strcmp(name, return_names[i]) == 0) {
                returning[i] = frame; found_return[i] = true;
            }
        }
    }
    fclose(file);
    for (size_t i = 0; i < 6; ++i)
        if (!found_track[i] || !found_return[i] ||
            (i && (track[i] < track[i - 1] ||
                returning[i] < returning[i - 1]))) return false;
    return true;
}
