#include "cubism_model.hpp"

#include <algorithm>
#include <cmath>

namespace bongo_cat {

/*
 * 把 frame (以画布比例表示的边距) 换算成像素视口。frame 描述"内容之外还要留多少", 所以
 * 内容尺寸 = 窗口尺寸 / (1 + 两侧边距之和); 结果钳进窗口并保证至少 1 像素, 免得极端
 * 比例下算出 0 或负数, 让 GL/Vulkan 的视口设置直接报错。
 */
void NativeModel::update_viewport() {
    float horizontal = 1.0f + frame_.left + frame_.right;
    float vertical = 1.0f + frame_.top + frame_.bottom;
    if (!std::isfinite(horizontal) || !std::isfinite(vertical) ||
        horizontal <= 0.0f || vertical <= 0.0f || width_ <= 0 || height_ <= 0) {
        viewport_x_ = viewport_y_ = 0;
        viewport_width_ = std::max(1, width_);
        viewport_height_ = std::max(1, height_);
        return;
    }
    float content_width = width_ / horizontal;
    float content_height = height_ / vertical;
    int left = (int)std::lround(frame_.left * content_width);
    int right = (int)std::lround(frame_.right * content_width);
    int bottom = (int)std::lround(frame_.bottom * content_height);
    int top = (int)std::lround(frame_.top * content_height);
    viewport_x_ = std::max(0, std::min(width_ - 1, left));
    viewport_y_ = std::max(0, std::min(height_ - 1, bottom));
    viewport_width_ = std::max(1,
        width_ - viewport_x_ - std::max(0, right));
    viewport_height_ = std::max(1,
        height_ - viewport_y_ - std::max(0, top));
}

bool NativeModel::frame(BongoCatLive2DFrame *frame) const {
    if (!_model || !frame) return false;
    *frame = frame_;
    return true;
}

bool NativeModel::viewport(int *x, int *y, int *width, int *height) const {
    if (!_model || !x || !y || !width || !height) return false;
    *x = viewport_x_;
    *y = viewport_y_;
    *width = viewport_width_;
    *height = viewport_height_;
    return true;
}

/* 把内容视口折进投影矩阵 (缩放 + 平移), 而不是去改视口设置: 叠加层 (背景/键盘/指针) 和
 * 模型必须落在同一块内容区域里才能对齐, 而两条渲染路径下发视口的方式不同, 折进矩阵是
 * 唯一一处两条路径都能共享的做法 (见 vk_backend.h 关于叠加层视口的说明)。 */
void NativeModel::apply_viewport_projection(
    Csm::CubismMatrix44 &projection) const {
    if (width_ <= 0 || height_ <= 0 || viewport_width_ <= 0 ||
        viewport_height_ <= 0) return;
    float scale_x = (float)viewport_width_ / (float)width_;
    float scale_y = (float)viewport_height_ / (float)height_;
    float translate_x = (2.0f * viewport_x_ + viewport_width_) /
        (float)width_ - 1.0f;
    float translate_y = (2.0f * viewport_y_ + viewport_height_) /
        (float)height_ - 1.0f;
    float *matrix = projection.GetArray();
    matrix[0] *= scale_x;
    matrix[4] *= scale_x;
    matrix[12] = matrix[12] * scale_x + translate_x;
    matrix[1] *= scale_y;
    matrix[5] *= scale_y;
    matrix[13] = matrix[13] * scale_y + translate_y;
}

/* 只统计数据、不改渲染: 给自动化测试和"模型到底画出来没有"的诊断用。顺手把可见包络经
 * 投影变换到 NDC 存下来, 渲染路径就不必再算一遍 (visual_state_ready_ 置位后外部才能读到)。 */
void NativeModel::record_visible_state(Csm::CubismMatrix44 &projection) {
    visual_state_.drawable_count = _model->GetDrawableCount();
    for (int i = 0; i < _model->GetDrawableCount(); ++i) {
        if (_model->GetDrawableDynamicFlagIsVisible(i) &&
            _model->GetDrawableOpacity(i) > 0.001f)
            ++visual_state_.drawable_visible;
        if (_model->GetDrawableDynamicFlagVertexPositionsDidChange(i))
            ++visual_state_.drawable_vertex_changed;
    }
    visual_state_.offscreen_count = _model->GetOffscreenCount();
    for (int i = 0; i < _model->GetOffscreenCount(); ++i)
        if (_model->GetOffscreenOpacity(i) > 0.001f)
            ++visual_state_.offscreen_positive;
    visual_state_.part_count = _model->GetPartCount();
    for (int i = 0; i < _model->GetPartCount(); ++i)
        if (_model->GetPartOpacity(i) > 0.001f)
            ++visual_state_.part_positive;
    ModelBounds bounds = capture_visible_bounds();
    if (!bounds.valid) return;
    float x0 = projection.TransformX(bounds.min_x);
    float x1 = projection.TransformX(bounds.max_x);
    float y0 = projection.TransformY(bounds.min_y);
    float y1 = projection.TransformY(bounds.max_y);
    visual_state_.visible_min_x = std::min(x0, x1);
    visual_state_.visible_max_x = std::max(x0, x1);
    visual_state_.visible_min_y = std::min(y0, y1);
    visual_state_.visible_max_y = std::max(y0, y1);
    visual_state_.visible = true;
}

bool NativeModel::visual_state(BongoCatLive2DVisualState *state) const {
    if (!state || !visual_state_ready_) return false;
    *state = visual_state_;
    return true;
}

} // namespace bongo_cat
