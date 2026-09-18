#include "bongo_cat/model.h"
#if defined(CSM_TARGET_WIN_GL) || defined(CSM_TARGET_LINUX_GL)
#include <GL/glew.h>
#endif
#include "cubism_runtime.hpp"

#include <SDL3/SDL_log.h>
#include <SDL3/SDL_video.h>
#include <exception>
#include <new>

static void retire_previous(BongoCatLive2D *runtime,
    bongo_cat::NativeModel *previous) {
    if (!runtime || !previous) return;
    if (runtime->retired_count == runtime->retired_capacity) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
            "Live2D retirement queue reached its limit; releasing the oldest model");
        delete runtime->retired[0].model;
        for (unsigned i = 1; i < runtime->retired_count; ++i)
            runtime->retired[i - 1] = runtime->retired[i];
        runtime->retired_count--;
    }
    BongoCatRetiredModel *slot = &runtime->retired[runtime->retired_count++];
    slot->model = previous;
    slot->frames_remaining = 3;
}

extern "C" BongoCatResult bongo_cat_live2d_load(BongoCatLive2D *runtime,
    const char *directory, const char *setting, bool preset,
    const BongoCatLive2DRenderOptions *render_options,
    BongoCatLive2DLoadProgress progress, void *userdata,
    BongoCatError *error) {
    if (!runtime) return BONGO_CAT_ERROR_ARGUMENT;
    bongo_cat::NativeModel *previous = runtime->model;
    bongo_cat::NativeModel *model = nullptr;
    try {
        model = new(std::nothrow) bongo_cat::NativeModel();
        if (!model) {
            bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY,
                "Cannot allocate Live2D model");
            return BONGO_CAT_ERROR_MEMORY;
        }
        if (render_options) model->set_render_options(*render_options);
        if (!model->load(directory, setting, preset, progress, userdata, error)) {
            delete model;
            return error ? error->code : BONGO_CAT_ERROR_CUBISM;
        }
        /* Keep the previous renderer alive until the replacement is complete. */
        model->reshape(runtime->width, runtime->height);
        if (!model->load_textures(error, progress, userdata)) {
            BongoCatResult result = error ? error->code : BONGO_CAT_ERROR_CUBISM;
            delete model;
            return result;
        }
        GLenum ready_error = glGetError();
        SDL_Log("[runtime] Live2D resource handoff: stage=replacement-ready "
            "new_textures=%zu previous_textures=%zu current_window=%p "
            "current_context=%p gl_error=0x%x",
            model->texture_count(), previous ? previous->texture_count() : 0,
            (void *)SDL_GL_GetCurrentWindow(),
            (void *)SDL_GL_GetCurrentContext(), (unsigned)ready_error);
        if (progress) progress(userdata, 1.0f);
        runtime->model = model;
        retire_previous(runtime, previous);
        SDL_Log("[runtime] Live2D resource handoff: stage=retirement-queued "
            "previous=%d previous_textures=%zu queue=%u frames=%u",
            previous != nullptr,
            previous ? previous->texture_count() : 0, runtime->retired_count,
            previous ? 3u : 0u);
        GLenum retired_error = glGetError();
        SDL_Log("[runtime] Live2D resource handoff: stage=complete "
            "new_textures=%zu current_window=%p current_context=%p gl_error=0x%x",
            model->texture_count(),
            (void *)SDL_GL_GetCurrentWindow(),
            (void *)SDL_GL_GetCurrentContext(), (unsigned)retired_error);
        return BONGO_CAT_OK;
    } catch (const std::bad_alloc &) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_MEMORY,
            "Out of memory while loading the Live2D model");
    } catch (const std::exception &exception) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_CUBISM,
            "Live2D model load failed: %s", exception.what());
    } catch (...) {
        bongo_cat_error_set(error, BONGO_CAT_ERROR_CUBISM,
            "Live2D model load failed with an unknown exception");
    }
    delete model;
    return error ? error->code : BONGO_CAT_ERROR_CUBISM;
}
