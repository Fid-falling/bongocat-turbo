#include "cubism_runtime.hpp"

extern "C" bool bongo_cat_live2d_visual_state(
    const BongoCatLive2D *runtime, BongoCatLive2DVisualState *state) {
    return runtime && runtime->model && runtime->model->visual_state(state);
}
