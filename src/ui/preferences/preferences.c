/*
 * 设置窗口的对外门面: 主题解析、页面资源生命周期、SDL 事件分流。
 *
 * 事件分流 (bongo_cat_preferences_event) 的顺序是有讲究的, 动之前先看这几条:
 * 1. 导入对话框与缓存目录异步对话框必须排在"按窗口 ID 过滤"之前 —— 它们是原生
 * 对话框推回来的 SDL 用户事件, 没有窗口 ID, 过了那道过滤就被丢掉, 历史症状
 * 就是"选完文件夹毫无反应";
 * 2. 时间戳早于 shown_ns 的事件要丢弃, 否则开窗那一瞬间会回放关窗期间排队下来
 * 的陈旧输入;
 * 3. ESC 分层处理: 行为设置对话框 > 删除确认框 > 关窗口。
 */
#include "bongo_cat/audio.h"
#include "bongo_cat/preferences.h"
#include "bongo_cat/app.h"
#include "bongo_cat/memory.h"
#include "bongo_cat/memory_policy.h"
#include "bongo_cat/platform.h"
#include "preferences_controls.h"
#include "preferences_model_cover.h"
#include "preferences_state.h"
#include "ui_animation.h"
#include "ui_catime.h"
#include "ui_native_theme.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <stdlib.h>

/* AUTO 跟随系统主题; 显式选了亮/暗就以设置为准, 不再看系统 */
int bongo_cat_preferences_resolved_theme(const BongoCatPreferences *value) {
    if (value->app->settings.app.theme == BONGO_CAT_THEME_DARK) return 1;
    if (value->app->settings.app.theme == BONGO_CAT_THEME_LIGHT) return 0;
    return SDL_GetSystemTheme() == SDL_SYSTEM_THEME_DARK;
}
/* 只在解析结果真的变了才重建样式和原生窗口主题: 两件事都不便宜, 而这里是每帧调的 */
void bongo_cat_preferences_apply_theme(BongoCatPreferences *value) {
    int dark = bongo_cat_preferences_resolved_theme(value);
    if (dark == value->style_theme) return;
    value->style_theme = dark;
    bongo_cat_ui_apply_theme(&value->ui.context, dark != 0);
    bongo_cat_ui_native_theme_apply(value->window, dark != 0);
}

/*
 * 切页时释放"只属于上一页"的资源。
 *
 * 职责划分: 这里只管随页面进退的纹理/资源缓存; 滚动位置不归它管 ——
 * scroll_current/scroll_target/scroll_ready 是按页号索引的数组, 每页各存一份,
 * 切页天然互不污染, 所以既不需要也不能在这里重置, 否则来回切页会丢掉用户
 * 滚到的位置。下标 3 是"关于"页, 它加载的支持者素材是四页里最重的, 离开时立刻
 * 归还, 并通过 memory_policy 汇报"UI 资源已卸载"。previous_page == next_page
 * 时是空操作。调用点在 draw_shell 检测到 page 变化的那一帧。
 */
void bongo_cat_preferences_page_cache_clear(BongoCatPreferences *value,
    int previous_page, int next_page) {
    if (!value || previous_page == next_page) return;
    bool released = false;
    if (previous_page == 3 && next_page != 3) {
        bongo_cat_preferences_support_assets_clear(value);
        released = true;
    }
    if (released) bongo_cat_memory_policy_ui_loaded();
}

/* 冒烟/自动化可用 app->smoke_preference_page 指定初始页, 直接跑到某一页做检查 */
BongoCatPreferences *bongo_cat_preferences_create(BongoCatApp *app) {
    if (!app) return NULL;
    BongoCatPreferences *value = calloc(1, sizeof(*value));
    if (value) { value->app = app;
        for (int i = 0; i < BONGO_CAT_OPTIONS_SECTION_COUNT; ++i)
            value->options_sections[i] = true;
        value->import_dialog = bongo_cat_preferences_import_create();
        if (!value->import_dialog) { free(value); return NULL; } }
    if (value && app->smoke_preference_page >= 0)
        value->page = app->smoke_preference_page;
    return value;
}

/* 只置请求标志, 真正的原生文件对话框由 present 在帧末打开: 不能在事件/绘制中途重入 */
void bongo_cat_preferences_request_model_import(BongoCatPreferences *value) {
    if (value && !bongo_cat_preferences_import_is_open(value->import_dialog))
        value->import_requested = true; }
bool bongo_cat_preferences_open_model_import(BongoCatPreferences *value,
    SDL_Window *parent) {
    return value && parent && bongo_cat_preferences_import_open(
        value->import_dialog, parent);
}
bool bongo_cat_preferences_visible(const BongoCatPreferences *value) {
    return value && value->window && value->visible;
}
/*
 * 轮询式"还需不需要下一帧", 是事件驱动渲染里唯一允许主动拉帧的地方。
 * 处理两类没有 SDL 事件可依赖的状态变化: 模型加载动画自然到点 (没有任何输入也会
 * 结束), 以及行为页上试听音频的开始/结束 —— 音频状态只有轮询才知道。返回 false
 * 时主循环就不再调 render, 设置窗口这时才真正闲下来 (进而被 release_idle_window 回收)。
 */
bool bongo_cat_preferences_needs_frame(BongoCatPreferences *value) {
    if (!value) return false;
    uint64_t now = SDL_GetTicksNS();
    if (value->model_load_visual_active &&
        !value->model_load_visual_completion_ns && !value->model_loading &&
        !value->model_selection_pending && now -
        value->model_load_visual_started_ns >=
            BONGO_CAT_MODEL_LOAD_VISUAL_DURATION_NS) {
        value->model_load_visual_active = false;
        value->model_load_progress = 1.0f;
        value->render_dirty = true;
    }
    if (!value->window || !value->visible) return false;
    if (value->behavior_dialog && value->app) {
        for (size_t i = 0; i < value->app->behaviors.count; ++i) {
            const BongoCatBehaviorEntry *entry = &value->app->behaviors.entries[i];
            bool playing = entry->kind == BONGO_CAT_BEHAVIOR_SOUND &&
                (entry->sound_clear ? bongo_cat_audio_any_playing(value->app->audio) :
                bongo_cat_audio_is_playing(value->app->audio, entry->sound));
            if (value->behavior_audio_playing[i] != playing) {
                value->behavior_audio_playing[i] = playing;
                value->render_dirty = true;
            }
        }
    }
    if (value->render_retry_ns > now) return false;
    bool raster_due = value->pending_raster_scale > 0.0f &&
        value->raster_retry_ns <= now;
    return value->render_dirty || raster_due || value->chrome_dragging; }
/* input_begin/input_end 必须成对包住一次 render: 输入收尾跟着渲染走, 不能提前 return */
void bongo_cat_preferences_input_begin(BongoCatPreferences *value) {
    if (!value || !value->window || !value->visible || value->input_active) return;
    bongo_cat_ui_input_begin(&value->ui);
    value->input_active = true; }
void bongo_cat_preferences_input_end(BongoCatPreferences *value) {
    if (!value || !value->window || !value->input_active) return;
    bongo_cat_ui_input_end(&value->ui);
    value->input_active = false;
}
/*
 * 取事件归属的窗口 ID。SDL_Event 各成员的字段名不统一, 只能逐个 case 取;
 * 没有窗口概念的事件 (用户事件等) 一律返回 0 —— 这类事件在调用方那边已经先
 * 处理掉了, 走不到这里。
 */
static Uint32 event_window(const SDL_Event *event) {
    if (event->type >= SDL_EVENT_WINDOW_FIRST && event->type <= SDL_EVENT_WINDOW_LAST)
        return event->window.windowID;
    switch (event->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: return event->key.windowID;
    case SDL_EVENT_TEXT_INPUT: return event->text.windowID;
    case SDL_EVENT_MOUSE_MOTION: return event->motion.windowID;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        return event->button.windowID;
    case SDL_EVENT_MOUSE_WHEEL: return event->wheel.windowID;
    case SDL_EVENT_DROP_BEGIN: case SDL_EVENT_DROP_FILE:
    case SDL_EVENT_DROP_COMPLETE: case SDL_EVENT_DROP_POSITION:
        return event->drop.windowID;
    default: return 0;
    }
}

/* 文件拖到窗口任意位置都切到"模型"页: 拖放的落点就是模型导入, 得让用户看见目标 */
static void enter_model_drop(BongoCatPreferences *value) {
    value->page = 1;
    value->import_drop_active = true;
    value->render_dirty = true;
}

/* 原生系统拖拽 (native_drag) 交给 SDL 自己处理; 弹了模态框就禁止拖窗, 否则会拖到遮罩底下 */
bool bongo_cat_preferences_chrome_drag_allowed(
    const BongoCatPreferences *value) {
    return value && value->app && !value->native_drag &&
        !bongo_cat_preferences_behavior_dialog_active(value) &&
        !bongo_cat_preferences_remove_dialog_active(value->app);
}

/*
 * 无边框窗口的标题栏拖拽。用全局鼠标坐标而不是窗口内坐标: 拖拽过程中指针经常已经
 * 跑到窗口外, 用窗口坐标会跳变; 另外在窗口外松开左键收不到 SDL_EVENT_MOUSE_BUTTON_UP,
 * 所以每帧用按键掩码兜底收尾, 否则会一直粘着鼠标。
 */
void bongo_cat_preferences_drag_tick(BongoCatPreferences *value) {
    if (!value || !value->window || !value->visible ||
        !value->chrome_dragging) return;
    float pointer_x = 0.0f, pointer_y = 0.0f;
    SDL_MouseButtonFlags buttons = SDL_GetGlobalMouseState(
        &pointer_x, &pointer_y);
    if (!(buttons & SDL_BUTTON_LMASK)) {
        SDL_CaptureMouse(false);
        value->chrome_dragging = false;
        return;
    }
    int current_x = 0, current_y = 0;
    if (!SDL_GetWindowPosition(value->window, &current_x, &current_y)) return;
    int next_x = value->drag_window_x +
        (int)(pointer_x - value->drag_pointer_x);
    int next_y = value->drag_window_y +
        (int)(pointer_y - value->drag_pointer_y);
    if (next_x != current_x || next_y != current_y)
        SDL_SetWindowPosition(value->window, next_x, next_y);
}

/* ESC 分层: 先关最上层的对话框, 都没有才关窗口 */
static bool chrome_event(BongoCatPreferences *value, const SDL_Event *event) {
    if (event->type == SDL_EVENT_KEY_DOWN && event->key.key == SDLK_ESCAPE) {
        if (bongo_cat_preferences_behavior_dialog_active(value)) {
            bongo_cat_preferences_behavior_dialog_close(value);
            value->render_dirty = true;
            return true;
        }
        if (bongo_cat_preferences_remove_dialog_active(value->app)) {
            bongo_cat_preferences_remove_dialog_close(value->app);
            value->render_dirty = true;
            return true;
        }
        bongo_cat_preferences_close(value);
        return true;
    }
    if (!bongo_cat_preferences_chrome_drag_allowed(value)) {
        if (event->type == SDL_EVENT_MOUSE_BUTTON_UP &&
            event->button.button == SDL_BUTTON_LEFT && value->chrome_dragging) {
            SDL_CaptureMouse(false);
            value->chrome_dragging = false;
            return true;
        }
        return false;
    }
    if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        event->button.button == SDL_BUTTON_LEFT) {
        int width = 0;
        SDL_GetWindowSize(value->window, &width, NULL);
        float scale = value->ui.layout_scale > 0.0f ? value->ui.layout_scale : 1.0f;
        if (!bongo_cat_ui_title_drag_hit(event->button.x / scale,
            event->button.y / scale, width / scale)) return false;
        SDL_GetWindowPosition(value->window, &value->drag_window_x,
            &value->drag_window_y);
        SDL_GetGlobalMouseState(&value->drag_pointer_x, &value->drag_pointer_y);
        value->chrome_dragging = true;
        if (!SDL_CaptureMouse(true))
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                "Mouse capture is unavailable during preferences drag: %s",
                SDL_GetError());
        return true;
    }
    if (event->type == SDL_EVENT_MOUSE_BUTTON_UP &&
        event->button.button == SDL_BUTTON_LEFT && value->chrome_dragging) {
        SDL_CaptureMouse(false);
        value->chrome_dragging = false;
        return true;
    }
    if (event->type != SDL_EVENT_MOUSE_MOTION || !value->chrome_dragging)
        return false;
    bongo_cat_preferences_drag_tick(value);
    return true;
}

bool bongo_cat_preferences_event(BongoCatPreferences *value, const SDL_Event *event) {
    if (!value || !event) return false;
    if (bongo_cat_preferences_import_event(value->import_dialog, value->app,
        event)) { value->render_dirty = value->visible;
        return true; }
    /*
     * "浏览"选择缓存目录的结果是异步对话框推回来的 SDL 用户事件: 它没有窗口 ID,
     * 一旦走到下面那道 event_window(...) != 本窗口 的过滤就会被丢掉 —— 这正是
     * "选完文件夹毫无反应"的原因, 所以必须在过滤之前处理。
     */
    if (bongo_cat_preferences_cache_path_dialog_event(value, event)) return true;
    if (!value->window) return false;
    if (event->type == SDL_EVENT_SYSTEM_THEME_CHANGED) {
        value->render_dirty = true; return false;
    }
    if (!value->visible) return false;
    if (event_window(event) != SDL_GetWindowID(value->window)) return false;
    if (value->shown_ns && event->common.timestamp &&
        event->common.timestamp < value->shown_ns) return true;
    if (event->type == SDL_EVENT_DROP_BEGIN) {
        enter_model_drop(value);
        return true;
    }
    if (event->type == SDL_EVENT_DROP_POSITION) {
        enter_model_drop(value);
        return true;
    }
    if (event->type == SDL_EVENT_DROP_COMPLETE) {
        value->import_drop_active = false;
        value->render_dirty = true;
        return true;
    }
    if (bongo_cat_preferences_scale_event(value, event)) return true;
#ifdef __APPLE__
    /* The user returns from System Settings through this window. */
    if (event->type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
        bongo_cat_preferences_input_monitoring_refresh(value);
        value->render_dirty = true;
    }
#endif
    if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        bongo_cat_ui_input_reset(&value->ui);
        if (value->chrome_dragging) SDL_CaptureMouse(false);
        value->chrome_dragging = false;
        value->import_drop_active = false;
        bongo_cat_pref_controls_reset(&value->ui.context);
    }
    if (bongo_cat_preferences_model_rename_event(value, event)) return true;
    if (bongo_cat_preferences_behavior_rename_event(value, event)) return true;
    if (bongo_cat_preferences_cache_path_event(value, event)) return true;
    if (bongo_cat_preferences_shortcut_event(value, event)) return true;
    if (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        bongo_cat_preferences_close(value);
        return true;
    }
    if (event->type == SDL_EVENT_DROP_FILE) {
        value->import_drop_active = false;
        bongo_cat_preferences_import_path(value->app, value->window, event->drop.data);
        return true;
    }
    if (chrome_event(value, event)) return true;
    if (!value->input_active) bongo_cat_preferences_input_begin(value);
    bongo_cat_ui_event(&value->ui, event);
    value->render_dirty = true;
    return true;
}

/* 外部改了设置 (托盘/配置文件/主窗口) 后调它, 就能让下一帧重画 */
void bongo_cat_preferences_invalidate(BongoCatPreferences *value) {
    if (value) value->render_dirty = true; }

/*
 * 模型目录变了: 先标脏重画, 再作废目录指纹 (watch_known=false 强制下次重新扫描),
 * 最后请求重建字形图集 —— 新模型的汉字必须先进图集, 否则卡片名字会画成缺字方块,
 * 所以这里刻意把 defer_once 清掉, 让重建赶在下一帧画卡片之前做, 而不是再等一帧。
 */
void bongo_cat_preferences_models_changed(BongoCatPreferences *value) {
    if (!value) return;
    value->render_dirty = true;
    value->model_directory_watch_known = false;
    value->font_reload_pending = value->ui_initialized;
    /* Catalog refreshes finish between UI frames. Rebuild before drawing the
       new cards so names never appear with missing-glyph placeholders. */
    value->font_reload_defer_once = false;
}
