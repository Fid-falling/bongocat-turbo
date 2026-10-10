/*
 * 薄接入层: 把 C 侧的 visual_state 查询转发给模型。
 *
 * 单独一个文件是为了让"诊断接口"不进主桥的翻译单元 —— 主桥被行数上限压着, 而这里只有
 * 一个转发函数, 加别的逻辑反而会让职责变模糊。runtime->model 可能为空 (还没加载模型),
 * 所以整条链都要短路。
 */
#include "cubism_runtime.hpp"

extern "C" bool bongo_cat_live2d_visual_state(
    const BongoCatLive2D *runtime, BongoCatLive2DVisualState *state) {
    return runtime && runtime->model && runtime->model->visual_state(state);
}
