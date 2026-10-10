/*
 * 冒烟与自动化用的自检代码。全部由 BONGO_CAT_TEST_* / smoke_* 开关控制, 默认路径
 * 上只有两个空判断的开销, 所以可以放心留在生产代码里。
 *
 * 它做两件在 CI 里没法用眼睛看的事:
 * 1. 把窗口句柄/页号/缩放等状态写到 state_root 下的文本文件, 外部脚本据此驱动
 * (比如给窗口发消息、截图);
 * 2. 检查这一帧"是不是真的画出来了" (bongo_cat_ui_frame_valid + 资源是否加载),
 * 第一次不合格就把 exit_code 置 1 —— 这正是 CI 判定设置窗口起没起来的依据。
 * 注意 Vulkan 下没有可读的 GL 前后缓冲, 角点透明度采样会被跳过。
 */
#include "preferences_state.h"
#include "ui_paint_cache.h"
#include "bongo_cat/file.h"
#include "bongo_cat/path.h"

#include <SDL3/SDL_opengl.h>
#include <stdint.h>
#include <stdio.h>

/*
 * 直接往事件队列里塞一对合成按键, 走的是和真实键盘完全相同的入口 ——
 * 测试快捷键录制时不能绕过 preferences_event, 否则测不到"录制会不会漏给全局"。
 */
static void send_key(BongoCatPreferences *value, Uint32 type, bool down) {
    SDL_Event event;
    SDL_zero(event);
    event.type = type;
    event.key.windowID = SDL_GetWindowID(value->window);
    event.key.down = down;
    event.key.scancode = SDL_SCANCODE_B;
    event.key.key = SDLK_B;
    event.key.mod = SDL_KMOD_CTRL | SDL_KMOD_SHIFT;
    bongo_cat_preferences_event(value, &event);
}

/*
 * 快捷键录制的自动化: 等用户真正点了录制框 (shortcut_recording 起来) 之后再合成按键。
 * 之所以还要等鼠标左键松开, 是因为点击本身会先被 pref_edit 消费; 提前送按键会被
 * 当成"点击之外的输入"丢掉。
 * 这个开关是一次性的 (发完就清), 否则每帧都会重新录一遍。
 */
void bongo_cat_preferences_shortcut_smoke(BongoCatPreferences *value) {
    if (!value || !value->window || !value->app->smoke_preference_shortcut ||
        !value->shortcut_recording ||
        value->ui.context.input.mouse.buttons[NK_BUTTON_LEFT].down) return;
    value->app->smoke_preference_shortcut = false;
    send_key(value, SDL_EVENT_KEY_DOWN, true);
    send_key(value, SDL_EVENT_KEY_UP, false);
}

static float font_height(const struct nk_user_font *font) {
    return font ? font->height : 0.0f;
}

/*
 * 每次渲染后都重写一遍, 而不是只在变化时写: 外部脚本是轮询读这个文件的, 文件必须
 * 始终反映"最后一帧"的状态; 句柄在 Windows 下才拿得到, 其它平台写 0 占位。
 */
static void write_window_handle(BongoCatPreferences *value) {
    if (!value->app->smoke_preferences) return;
    uintptr_t native = 0;
#ifdef _WIN32
    native = (uintptr_t)SDL_GetPointerProperty(
        SDL_GetWindowProperties(value->window),
        SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
#endif
    char path[BONGO_CAT_PATH_CAP];
    if (!bongo_cat_path_join(path, sizeof(path), value->app->state_root,
        "preferences-window.txt")) return;
    FILE *file = bongo_cat_file_open(path, "wb");
    if (!file) return;
    fprintf(file, "handle=%llu window_id=%u page=%d recording=%d shortcut_smoke=%d\n",
        (unsigned long long)native, (unsigned)SDL_GetWindowID(value->window),
        value->page, value->shortcut_recording,
        value->app->smoke_preference_shortcut);
    fclose(file);
}

/*
 * 帧计时采样: 每次 present 追加一行时间戳, 供脚本算帧间隔分布。
 * 用追加模式打开并逐行关闭, 是为了"进程被强杀也不会丢已写的数据"。
 */
void bongo_cat_preferences_record_frame(BongoCatPreferences *value) {
    if (!value->app->smoke_frame_series) return;
    char path[BONGO_CAT_PATH_CAP];
    bongo_cat_path_join(path, sizeof(path), value->app->state_root,
        "preferences-frames.csv");
    FILE *file = bongo_cat_file_open(path, "ab");
    if (!file) return;
    fprintf(file, "%llu\n", (unsigned long long)SDL_GetTicksNS());
    fclose(file);
}

/*
 * 帧自检 + 状态落盘 (在 UI 画完、present 之前调用: 这时帧缓冲里还是刚画好的内容)。
 * frame_checked 保证只判第一帧 —— 首帧画不出来就说明窗口/字体/资源初始化有硬伤,
 * 后面再判只会掩盖真正的原因。
 * 第 3 页 (关于) 额外要求支持者素材到位, 因为它是最容易因为文件缺失而空掉的一页。
 */
void bongo_cat_preferences_smoke_frame(BongoCatPreferences *value) {
    if (!value) return;
    write_window_handle(value);
    if (!value->app->smoke) return;
    bool valid = bongo_cat_ui_frame_valid(&value->ui);
    bool assets_valid = value->logo_texture && value->icon_texture &&
        (value->page != 3 ||
            (value->catime_texture && value->vlaina_texture));
    if (!value->frame_checked) {
        value->frame_checked = true;
        if (!valid || !assets_valid) value->app->exit_code = 1;
    }
    int window_width = 0, window_height = 0;
    int pixel_width = 0, pixel_height = 0;
    float logical_width = 0.0f, logical_height = 0.0f;
    SDL_GetWindowSize(value->window, &window_width, &window_height);
    SDL_GetWindowSizeInPixels(value->window, &pixel_width, &pixel_height);
    bongo_cat_ui_logical_size(&value->ui, &logical_width, &logical_height);
    unsigned char corners[4][4] = {{0}};
    /* Vulkan 模式下没有可读的 GL 前后缓冲, 角点透明度采样暂时跳过 */
    if (!bongo_cat_ui_uses_vulkan(&value->ui) &&
        pixel_width > 0 && pixel_height > 0) {
        const int points[4][2] = {{0, 0}, {pixel_width - 1, 0},
            {0, pixel_height - 1}, {pixel_width - 1, pixel_height - 1}};
        for (int i = 0; i < 4; ++i) glReadPixels(points[i][0], points[i][1],
            1, 1, GL_RGBA, GL_UNSIGNED_BYTE, corners[i]);
    }
    unsigned corner_alpha = 0;
    for (int i = 0; i < 4; ++i)
        if (corners[i][3] > corner_alpha) corner_alpha = corners[i][3];
    char path[BONGO_CAT_PATH_CAP];
    bongo_cat_path_join(path, sizeof(path), value->app->state_root,
        "ui-frame.txt");
    FILE *file = bongo_cat_file_open(path, "wb");
    if (!file) return;
    size_t paint_count = 0;
    size_t paint_bytes = bongo_cat_ui_paint_cache_usage(&value->ui,
        &paint_count);
    fprintf(file, "valid=%d page=%d convert=%d vertices=%zu elements=%zu "
        "commands=%u draw_elements=%u gl_error=%u alpha_vertices=%u "
        "max_alpha=%u font_path=%d font_file=%d custom_font=%d font_probe=%d "
        "layout_scale=%.3f raster_scale=%.3f window=%dx%d pixels=%dx%d "
        "logical=%.2fx%.2f atlas=%dx%d transparent=%d corner_alpha=%u "
        "resize_cached=%u resize_failures=%u resize_layout=%u "
        "fonts=%.1f,%.1f,%.1f,%.1f,%.1f "
        "paint_textures=%zu paint_bytes=%zu "
        "assets=%u,%u,%u,%u,%u valid_assets=%d\n",
        valid, value->page, value->ui.last_convert_result,
        value->ui.last_vertex_bytes, value->ui.last_element_bytes,
        value->ui.last_draw_commands, value->ui.last_draw_elements,
        (unsigned)value->ui.last_gl_error, value->ui.nonzero_alpha_vertices,
        value->ui.max_alpha, value->ui.font_path_found,
        value->ui.font_file_loaded, value->ui.custom_font_loaded,
        value->ui.font_probe_loaded, value->ui.layout_scale,
        value->ui.raster_scale, window_width, window_height,
        pixel_width, pixel_height, logical_width, logical_height,
        value->ui.font_atlas_width, value->ui.font_atlas_height,
        value->transparent_window, corner_alpha,
        value->ui.resize_cache_presentations, value->ui.resize_cache_failures,
        value->live_resize_layout_frames,
        font_height(value->ui.caption_font), font_height(value->ui.body_font),
        font_height(value->ui.label_font), font_height(value->ui.heading_font),
        font_height(value->ui.hero_font), paint_count, paint_bytes,
        value->logo_texture, value->icon_texture,
        value->icon_texture_hidpi, value->catime_texture,
        value->vlaina_texture, assets_valid);
    fclose(file);
}
