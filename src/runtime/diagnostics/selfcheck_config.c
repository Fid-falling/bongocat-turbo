/*
 * 自检第 5 组: 配置, 以及第 2 组用到的"渲染设置摘要"。
 *
 * 这里刻意把 settings.json **重新解析一遍**, 而不是直接读内存里的 app->settings:
 * 内存里的值已经过 bongo_cat_settings_validate 钳制, 越界的原始值在那一步就被抹掉了。
 * 用户手改过配置 (或旧版本留下奇怪的数) 时, "文件里写了什么、被钳制成了什么"正是
 * 排查画面不对的第一手证据。自检只读文件, 不做任何写回。
 */
#include "selfcheck_internal.h"

#include "bongo_cat/app.h"
#include "bongo_cat/config.h"
#include "bongo_cat/json.h"
#include "bongo_cat/path.h"

#include <stdio.h>
#include <string.h>

static const char *quality_name(int quality) {
    switch (quality) {
    case 0: return "标准";
    case 1: return "高";
    case 2: return "原生";
    default: return "未知档位";
    }
}

void bongo_cat_selfcheck_render_settings(const struct BongoCatApp *app,
    char *output, size_t capacity) {
    if (!output || capacity == 0) return;
    if (!app) {
        snprintf(output, capacity, "无法读取（应用状态未就绪）");
        return;
    }
    const BongoCatModelPreferences *model = &app->settings.model;
    char fps[32];
    if (model->max_fps <= 0)
        snprintf(fps, sizeof(fps), "不限");
    else
        snprintf(fps, sizeof(fps), "%d FPS", model->max_fps);
    snprintf(output, capacity,
        "渲染精度 %.1fx · 纹理质量 %s (档 %d) · 最大帧率 %s · 垂直同步 %s · "
        "渲染后端(配置) %s",
        (double)model->render_scale, quality_name(model->texture_quality),
        model->texture_quality, fps, model->vsync ? "开" : "关",
        bongo_cat_renderer_name(model->renderer));
}

/* 只报"越界/类型不对"的项: 这些是会被 bongo_cat_settings_validate 改写的值。
   返回发现的条数, 并把前几条拼进 note。 */
static int collect_invalid_values(yyjson_val *root, char *note,
    size_t capacity) {
    int found = 0;
    size_t used = 0;
    note[0] = '\0';
    yyjson_val *rendering = yyjson_obj_get(root, "rendering");
    yyjson_val *sound = yyjson_obj_get(root, "sound");
    struct {
        char name[32];
        bool bad;
        char text[96];
    } checks[5];
    memset(checks, 0, sizeof(checks));
    double scale = 1.0;
    yyjson_val *value = rendering ? yyjson_obj_get(rendering, "renderScale") : NULL;
    if (value && yyjson_is_num(value)) {
        scale = yyjson_get_num(value);
        checks[0].bad = scale < 0.1 || scale > 4.0;
        snprintf(checks[0].name, sizeof(checks[0].name), "renderScale");
        snprintf(checks[0].text, sizeof(checks[0].text), "%.3g (允许 0.1–4.0)",
            scale);
    }
    value = rendering ? yyjson_obj_get(rendering, "textureQuality") : NULL;
    if (value && yyjson_is_int(value)) {
        long long quality = yyjson_get_sint(value);
        checks[1].bad = quality < 0 || quality > 2;
        snprintf(checks[1].name, sizeof(checks[1].name), "textureQuality");
        snprintf(checks[1].text, sizeof(checks[1].text), "%lld (允许 0–2)", quality);
    }
    value = rendering ? yyjson_obj_get(rendering, "maximumFps") : NULL;
    if (value && yyjson_is_int(value)) {
        long long fps = yyjson_get_sint(value);
        checks[2].bad = fps < 0;
        snprintf(checks[2].name, sizeof(checks[2].name), "maximumFps");
        snprintf(checks[2].text, sizeof(checks[2].text), "%lld (不能为负)", fps);
    }
    value = rendering ? yyjson_obj_get(rendering, "vsync") : NULL;
    if (value && yyjson_is_int(value)) {
        long long vsync = yyjson_get_sint(value);
        checks[3].bad = vsync != 0 && vsync != 1;
        snprintf(checks[3].name, sizeof(checks[3].name), "vsync");
        snprintf(checks[3].text, sizeof(checks[3].text), "%lld (只能是 0/1)", vsync);
    }
    value = sound ? yyjson_obj_get(sound, "volume") : NULL;
    if (value && yyjson_is_int(value)) {
        long long volume = yyjson_get_sint(value);
        checks[4].bad = volume < 0 || volume > 100;
        snprintf(checks[4].name, sizeof(checks[4].name), "sound.volume");
        snprintf(checks[4].text, sizeof(checks[4].text), "%lld (允许 0–100)", volume);
    }
    for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); ++i) {
        if (!checks[i].bad) continue;
        ++found;
        if (found > 3) continue; /* 面板一行放不下更多, 报告里也只举最典型的三条 */
        int written = snprintf(note + used, capacity - used, "%s%s=%s",
            used ? "; " : "", checks[i].name, checks[i].text);
        if (written < 0 || (size_t)written >= capacity - used) break;
        used += (size_t)written;
    }
    return found;
}

void bongo_cat_selfcheck_config(BongoCatSelfCheckBuilder *builder,
    struct BongoCatApp *app) {
    bongo_cat_selfcheck_section(builder, "配置");
    const char *path = app ? app->settings_path : NULL;
    yyjson_read_err error = {0};
    if (!path || !path[0]) {
        bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_FAIL, "settings.json",
            "配置路径未解析，设置无法保存");
    } else if (!bongo_cat_path_is_file(path)) {
        bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_WARN, "settings.json",
            "尚未生成（%s）：当前使用的是默认设置", path);
    } else {
        yyjson_doc *document = bongo_cat_json_read_file(path, 0, &error);
        if (!document)
            bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_FAIL,
                "settings.json", "无法解析：%s（位置 %zu, %s）",
                error.msg ? error.msg : "unknown error", error.pos,
                path);
        else {
            char note[256];
            int invalid = collect_invalid_values(yyjson_doc_get_root(document),
                note, sizeof(note));
            yyjson_doc_free(document);
            uint64_t size = 0;
            bongo_cat_path_file_size(path, &size);
            bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_PASS,
                "settings.json", "已加载并可解析（%llu 字节）：%s",
                (unsigned long long)size, path);
            if (invalid)
                bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_WARN,
                    "非法值", "发现 %d 处越界值，已被钳制到合法范围：%s",
                    invalid, note);
            else
                bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_PASS,
                    "非法值", "没有发现越界值");
        }
    }
    char settings[256];
    bongo_cat_selfcheck_render_settings(app, settings, sizeof(settings));
    BongoCatSoundPreferences *sound = app ? &app->settings.sound : NULL;
    char text[384];
    snprintf(text, sizeof(text), "%s · 音效 %s（音量 %d）", settings,
        sound && sound->muted ? "静音" : "开", sound ? sound->volume : 0);
    bongo_cat_selfcheck_add(builder, BONGO_CAT_SELFCHECK_PASS, "关键项当前值",
        "%s", text);
}
