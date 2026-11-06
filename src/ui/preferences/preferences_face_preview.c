/*
 * 面捕预览窗: 把采集线程的最近一帧画在设置页里 (类似 VTube Studio 的预览)。
 *
 * 为什么绕一个 PNG 文件: 设置界面有 Vulkan / OpenGL 两条渲染路径, 各自的纹理对象不一样,
 * 而"从文件加载纹理"这条项目里已经有了且两条后端都支持 (bongo_cat_ui_texture_from_file)。
 * 预览帧只有 240x180, 4fps 写一次 PNG 的代价, 远小于再写一套双后端的内存纹理上传。
 *
 * 数据流:
 *   采集线程: 每 ~5 帧把当前帧缩到 240x180 存进 preview 缓冲 (带代数)
 *   设置页:   代数变了才重新导出 PNG, 并让 texture_from_file 重新加载
 */
#include "preferences_state.h"

#include "preferences_widgets.h"

#include "../../runtime/input/face_capture.h"

#include "bongo_cat/log.h"
#include "bongo_cat/path.h"

#include <SDL3/SDL.h>
#include <stb_image_write.h>

#include <stdio.h>
#include <string.h>

#define PREVIEW_WIDTH 240
#define PREVIEW_HEIGHT 180

static unsigned int preview_texture = 0;
static uint32_t preview_generation = 0;
static BongoCatApp *preview_owner = NULL;
static char preview_path[BONGO_CAT_PATH_CAP];

static void write_to_file(void *context, void *data, int size) {
    SDL_IOStream *io = context;
    if (io) SDL_WriteIO(io, data, (size_t)size);
}

/* 写 PNG: 临时文件 + 改名, 读的一方永远看不到半截文件 */
static bool write_preview_png(const char *path, const unsigned char *rgb) {
    char temporary[BONGO_CAT_PATH_CAP];
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    SDL_IOStream *io = SDL_IOFromFile(temporary, "wb");
    if (!io) return false;
    const bool ok = stbi_write_png_to_func(write_to_file, io, PREVIEW_WIDTH,
        PREVIEW_HEIGHT, 3, rgb, PREVIEW_WIDTH * 3) != 0;
    SDL_CloseIO(io);
    if (!ok) { remove(temporary); return false; }
    remove(path);
    return rename(temporary, path) == 0;
}

/* 预览图落在缓存目录里 (每次运行覆盖同一个文件) */
static bool preview_file_path(BongoCatApp *app, char *output, size_t capacity) {
    if (!app || !app->cache_root[0]) return false;
    return bongo_cat_path_join(output, capacity, app->cache_root,
        "face-capture-preview.png");
}

/*
 * 画预览窗。没有面捕运行时画一行提示, 让用户知道这里本来会显示什么。
 * 返回 true = 画了图像。
 */
bool bongo_cat_preferences_face_preview(BongoCatPreferences *value,
    struct nk_context *context) {
    if (!value || !value->app) return false;
    BongoCatApp *app = value->app;

    const bool running = app->face_capture != NULL;
    if (!running) {
        bongo_cat_pref_status(context, "face-preview-status",
            "预览", "面捕未运行");
        return false;
    }

    if (preview_owner != app || !preview_path[0]) {
        preview_owner = app;
        if (!preview_file_path(app, preview_path, sizeof(preview_path)))
            preview_path[0] = 0;
    }
    static unsigned char frame[PREVIEW_WIDTH * PREVIEW_HEIGHT * 3];
    uint32_t generation = 0;
    const bool has_frame = bongo_cat_face_capture_preview(app->face_capture, frame,
        PREVIEW_WIDTH, PREVIEW_HEIGHT, &generation);
    if (has_frame && preview_path[0] && generation != preview_generation) {
        BongoCatError error = {0};
        if (write_preview_png(preview_path, frame)) {
            const unsigned int texture = (unsigned int)bongo_cat_ui_texture_from_file(
                &value->ui, preview_path, PREVIEW_WIDTH, PREVIEW_HEIGHT, NULL, NULL,
                &error);
            if (texture) {
                if (preview_texture)
                    bongo_cat_ui_texture_release_id(&value->ui, (int)preview_texture);
                preview_texture = texture;
                preview_generation = generation;
            }
        }
    }

    if (!preview_texture) {
        bongo_cat_pref_status(context, "face-preview-status", "预览", "等待画面…");
        return false;
    }
    /* 画图: 固定 240x180, 靠左, 和表单行的图标列对齐 */
    nk_layout_row_dynamic(context, (float)PREVIEW_HEIGHT, 1);
    struct nk_rect bounds;
    if (nk_widget(&bounds, context) == NK_WIDGET_INVALID) return false;
    const struct nk_image image = nk_image_id((int)preview_texture);
    nk_draw_image(nk_window_get_canvas(context), bounds, &image,
        nk_rgba(255, 255, 255, 255));
    return true;
}

/* 离开面捕页/停止面捕时释放纹理 (页面缓存清理时调用) */
void bongo_cat_preferences_face_preview_release(BongoCatPreferences *value) {
    if (!value) return;
    if (preview_texture) {
        bongo_cat_ui_texture_release_id(&value->ui, (int)preview_texture);
        preview_texture = 0;
    }
    preview_generation = 0;
    preview_owner = NULL;
}
