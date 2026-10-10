#include "cubism_model.hpp"

#include <Motion/CubismExpressionMotionManager.hpp>
#include <Motion/CubismMotionQueueEntry.hpp>
#include <cmath>

namespace bongo_cat {

/*
 * index == -1 表示清除表情。清除不是硬切: 正在淡入的表情要先把淡出时间设成它自己的淡出
 * 时长, 让 SDK 平滑收尾 —— 直接 StopAllMotions 会让脸"啪"地跳回默认表情。
 * expression_clearing_ 记住"还有表情在淡出", 由 expire_expression_fade() 收尾。
 */
bool NativeModel::set_expression(int index) {
    if (index == -1) {
        expression_index_ = -1;
        expression_frame_pending_ = false;
        auto *entries = _expressionManager->GetCubismMotionQueueEntries();
        /* 队列里没有条目 (表情还没真正开始播) 就直接停, 否则是在给一个空队列设淡出。 */
        expression_clearing_ = entries && entries->GetSize() > 0;
        if (!expression_clearing_) {
            _expressionManager->StopAllMotions();
            return true;
        }
        for (Csm::csmUint32 i = 0; i < entries->GetSize(); ++i) {
            Csm::CubismMotionQueueEntry *entry = entries->At(i);
            if (!entry) continue;
            Csm::ACubismMotion *motion = entry->GetCubismMotion();
            if (motion) {
                float fade_out = motion->GetFadeOutTime();
                entry->SetFadeout(fade_out < 0.0f ? 0.0f : fade_out);
            }
        }
        return true;
    }
    if (index < 0 || (size_t)index >= expression_names_.size()) return false;
    auto found = expressions_.find(expression_names_[(size_t)index]);
    if (found == expressions_.end()) return false;
    Csm::CubismMotionQueueEntryHandle handle = _expressionManager->StartMotion(
        found->second, false);
    if (handle == Csm::InvalidMotionQueueEntryHandleValue) return false;
    expression_clearing_ = false;
    expression_index_ = index;
    expression_frame_pending_ = true;
    return true;
}

/* 封面截帧只画一帧, 没有后续帧把表情的淡入推进完, 所以这里手动推两次时钟: 一次 0 秒让
 * 表情真正入队生效, 一次一整个淡入时长让它到终态。 */
void NativeModel::settle_pending_expression_for_cover() {
    if (!expression_frame_pending_ || !_model || expression_index_ < 0 ||
        (size_t)expression_index_ >= expression_names_.size()) return;
    auto found = expressions_.find(expression_names_[(size_t)expression_index_]);
    if (found == expressions_.end()) {
        expression_frame_pending_ = false;
        return;
    }
    float fade_seconds = found->second->GetFadeInTime();
    if (!std::isfinite(fade_seconds) || fade_seconds < 0.0f)
        fade_seconds = 0.0f;
    /* Only the expression clock is advanced. The model baseline already
       contains restored persistent motions, while idle effects stay put. */
    _model->LoadParameters();
    _expressionManager->UpdateMotion(_model, 0.0f);
    if (fade_seconds > 0.0f)
        _expressionManager->UpdateMotion(_model, fade_seconds);
    expression_frame_pending_ = false;
}

/* 淡出收尾: 每帧轮询队列条目的结束时间, 全部走完才真正 StopAllMotions。不交给 SDK 自行
 * 回收, 是因为我们要在"最后一个表情消失"的那一刻把 expression_clearing_ 复位, 否则状态机
 * 会一直卡在这个中间态上。 */
void NativeModel::expire_expression_fade() {
    if (!expression_clearing_) return;
    auto *entries = _expressionManager->GetCubismMotionQueueEntries();
    if (!entries || entries->GetSize() == 0) {
        expression_clearing_ = false;
        return;
    }
    for (Csm::csmUint32 i = 0; i < entries->GetSize(); ++i) {
        Csm::CubismMotionQueueEntry *entry = entries->At(i);
        if (entry && (entry->GetEndTime() < 0.0f ||
            entry->GetStateTime() < entry->GetEndTime())) return;
    }
    _expressionManager->StopAllMotions();
    expression_clearing_ = false;
}

} // namespace bongo_cat
