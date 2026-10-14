/*
 * 模型切换的"影子准备"阶段。
 *
 * 一次切换被拆成三段: 影子准备 (本文件) -> 分步加载 (cubism_bridge_load_session.cpp) ->
 * 提交 (model_files.c)。拆开的原因是加载要花 1~5 秒, 而主循环必须在这期间持续出帧;
 * 于是"哪些必须在切换开始的那一刻定下来"和"哪些必须等新模型真就绪才能做"被分成两段。
 *
 * 本文件负责前者: 结束快照/捕获行为状态/刷新旧模型封面/解析行为目录与适配器 profile/
 * 记录窗口与 GL 上下文指针/激活主上下文。每一步都只读当前状态、不做不可逆修改, 所以它随时
 * 可以被丢弃 —— 丢弃时会把临时分配还掉, 并用与进入时相同的方式还原 GL 上下文。
 *
 * 上下文约定 (与一次性版本完全一致): 只有 OpenGL 后端需要临时切到主窗口的上下文, Vulkan
 * 没有 GL 上下文因此整段跳过; 还原用的窗口/上下文在准备时记下, 在释放时使用。
 */
#include "runtime.h"
#include "model_switch_shadow.h"
#include "model_behavior_cache.h"
#include "model_geometry.h"
#include "model_import.h"
#include "model_cover.h"
#include "bongo_cat/render.h"

#include <SDL3/SDL_opengl.h>
#include <stdlib.h>

/* 当前是否走 OpenGL 后端 (Vulkan 不读 glGetError, 也不切上下文)。 */
bool bongo_cat_model_switch_gl_backend(void) {
    return bongo_cat_renderer_active() != BONGO_CAT_RENDERER_VULKAN;
}

/* 进入影子准备: 前置检查 + 快照收尾 + 行为目录 + 适配器 profile + 上下文。
 * 返回 false 时已经填好 *error, 调用方直接失败返回即可 (此时 shadow 里没有需要释放的东西,
 * 因为失败都发生在分配之前或由本函数自己回收)。
 */
bool bongo_cat_model_switch_prepare(BongoCatApp *app,
    const BongoCatModelEntry *entry, BongoCatModelSwitchShadow *shadow,
    BongoCatError *error) {
    if (!app || !entry || !shadow) return false;
    *shadow = (BongoCatModelSwitchShadow){0};
    /* 快照预览要先补画最后一帧实时画面才能释放, 所以必须在切换模型与 GL 上下文之前结束它,
     * 否则补画用的是已经被替换掉的状态。 */
    bongo_cat_window_snapshot_end(app);
    bongo_cat_app_capture_behavior_state(app);
    shadow->replacing = app->loaded_model[0] != '\0';
    if (shadow->replacing) bongo_cat_model_cover_capture_before_switch(app);
    /* 行为目录优先复用 app->behavior_cache (仅限已安装包且内容摘要匹配), 否则重新解析。
     * 解析失败不是致命错误: 模型照样显示, 只是没有动作与表情; 这里只记下 validity 供缓存
     * 判定使用。 */
    shadow->behaviors = calloc(1, sizeof(*shadow->behaviors));
    if (!shadow->behaviors) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY,
            "Cannot allocate model behavior state");
        return false;
    }
    BongoCatError optional = {0};
    shadow->behavior_catalog_valid = bongo_cat_model_behavior_cache_matches(
        app, entry);
    if (shadow->behavior_catalog_valid) {
        *shadow->behaviors = *app->behavior_cache;
    } else {
        shadow->behavior_catalog_valid = bongo_cat_behaviors_load(
            shadow->behaviors, entry, &optional) == BONGO_CAT_OK;
        if (!shadow->behavior_catalog_valid)
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s", optional.message);
    }
    /* 适配器 profile 是导入时生成的运行时参数 (投影、指针边界等); 读不到就用零初始化的
     * 默认值。它同时会写进 app->model_render_options 供后续帧复用。 */
    if (bongo_cat_import_render_options(entry->adapter_directory,
            &shadow->render_options))
        SDL_Log("Imported runtime profile: projection=%.4f force_mouse=%d "
            "left_handed=%d pointer_bounds=%d", shadow->render_options.projection_scale,
            shadow->render_options.mouse_force_move,
            shadow->render_options.pointer_left_handed,
            shadow->render_options.custom_pointer_bounds);
    shadow->content_anchor = bongo_cat_model_content_anchor(app);
    shadow->pixel_width = app->session.window.width;
    shadow->pixel_height = app->session.window.height;
    if (app->window)
        SDL_GetWindowSizeInPixels(app->window, &shadow->pixel_width,
            &shadow->pixel_height);
    shadow->previous_window = SDL_GL_GetCurrentWindow();
    shadow->previous_context = SDL_GL_GetCurrentContext();
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Model switch shadow: path=%s previous=%s behaviors_valid=%d",
        entry->id, shadow->replacing ? app->loaded_model : "none",
        shadow->behavior_catalog_valid);
    return true;
}

/* 把主窗口的 GL 上下文切进来 (Vulkan 后端不需要, 直接返回真)。必须在分步加载的第一帧之前
 * 调用; 它只做一次, 之后靠激活标记保证不重复切。 */
bool bongo_cat_model_switch_activate(BongoCatApp *app,
    BongoCatModelSwitchShadow *shadow, BongoCatError *error) {
    if (!app || !shadow) return false;
    if (!bongo_cat_model_switch_gl_backend()) return true;
    if (shadow->context_activated) return true;
    if (SDL_GL_GetCurrentWindow() == app->window &&
        SDL_GL_GetCurrentContext() == (SDL_GLContext)app->gl_context)
        return true;
    if (!SDL_GL_MakeCurrent(app->window, (SDL_GLContext)app->gl_context)) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_PLATFORM,
            "Cannot activate the main OpenGL context: %s", SDL_GetError());
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", error->message);
        return false;
    }
    shadow->context_activated = true;
    return true;
}

/* 还回进入时记下的 GL 上下文。成功与失败两条出口都要调 (幂等), 否则把调用方的 GL 状态
 * 改掉之后, 后续所有 GL 调用都会打到别的上下文上。 */
void bongo_cat_model_switch_release(BongoCatApp *app,
    BongoCatModelSwitchShadow *shadow) {
    (void)app;
    if (!shadow) return;
    if (shadow->context_activated && shadow->previous_window &&
        shadow->previous_context &&
        !SDL_GL_MakeCurrent(shadow->previous_window,
            (SDL_GLContext)shadow->previous_context))
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Cannot restore the previous OpenGL context after a model switch: %s",
            SDL_GetError());
    shadow->context_activated = false;
    free(shadow->behaviors);
    shadow->behaviors = NULL;
}
