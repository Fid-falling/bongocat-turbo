/*
 * 动作的"试听/预览"事务。
 *
 * 设置窗口里点一个动作只是先看看效果, 用户没确认就不该留下痕迹 —— 但播放动作必然会改
 * 参数、部件不透明度和整模型不透明度。所以预览开始前把这三样全量快照, 之后要么 commit
 * (保留结果并同步勾选态)、要么 restore (整体回滚)。预览不嵌套: 已有活跃预览时再预览
 * 别的动作, 会先回滚到原点再开始。
 */
#include "cubism_model.hpp"

#include <Motion/CubismMotionManager.hpp>

namespace bongo_cat {

/* 抄参数时先摘掉外部输入覆盖: 要记的是"动作基线", 不是被覆盖之后显示出来的值。
 * LoadParameters() 把参数拉回 Cubism 自己保存的基线, 抄完再恢复刚才的显示值与覆盖状态,
 * 这样这次快照不会让画面跳一下。 */
void NativeModel::capture_motion_preview() {
    if (!_model || motion_preview_active_) return;
    const int parameter_count = _model->GetParameterCount();
    const bool overrides_were_applied = parameter_overrides_applied_;
    std::vector<float> current_parameters((size_t)parameter_count);
    for (int i = 0; i < parameter_count; ++i)
        current_parameters[(size_t)i] = _model->GetParameterValue(i);
    _model->LoadParameters();
    parameter_overrides_applied_ = false;
    capture_parameter_baseline();
    motion_preview_parameters_.resize((size_t)parameter_count);
    for (int i = 0; i < parameter_count; ++i) {
        motion_preview_parameters_[(size_t)i] = _model->GetParameterValue(i);
        _model->SetParameterValue(i, current_parameters[(size_t)i]);
    }
    if (overrides_were_applied) apply_parameter_overrides();
    /* 部件不透明度和整模型不透明度 SDK 不会帮着回滚, 必须自己一起快照。 */
    const int part_count = _model->GetPartCount();
    motion_preview_parts_.resize((size_t)part_count);
    for (int i = 0; i < part_count; ++i)
        motion_preview_parts_[(size_t)i] = _model->GetPartOpacity(i);
    motion_preview_opacity_ = _model->GetModelOpacity();
    motion_preview_active_ = true;
}

/* 回滚顺序有讲究: 先停掉所有动作并清记账, 否则紧接着那一帧动作还会继续写参数, 把刚
 * 恢复好的值又盖回去。之后才轮到参数/部件/不透明度, 最后重新应用外部输入覆盖。 */
void NativeModel::restore_motion_preview_state() {
    if (!_model || !motion_preview_active_) return;
    _motionManager->StopAllMotions();
    clear_motion_runs();
    parameter_overrides_applied_ = false;
    const int parameter_count = _model->GetParameterCount();
    for (int i = 0; i < parameter_count &&
        (size_t)i < motion_preview_parameters_.size(); ++i)
        _model->SetParameterValue(i, motion_preview_parameters_[(size_t)i]);
    save_parameters();
    const int part_count = _model->GetPartCount();
    for (int i = 0; i < part_count &&
        (size_t)i < motion_preview_parts_.size(); ++i)
        _model->SetPartOpacity(i, motion_preview_parts_[(size_t)i]);
    _model->SetModelOpacity(motion_preview_opacity_);
    _opacity = motion_preview_opacity_;
    apply_parameter_overrides();
}

/* 已经有预览在跑时, 先把画面恢复到预览之前的状态再开始新的 —— 所以连续预览多个动作
 * 不会互相叠加, 每次都从同一个原点出发。开关对里的一侧按 owner 归一化, 于是"预览开"
 * 之后点"预览关"会被识别成同一个事务。 */
bool NativeModel::preview_motion(const char *group, int index) {
    if (!_model || !group || index < 0) return false;
    if (motion_preview_active_) restore_motion_preview_state();
    else capture_motion_preview();
    std::string key = std::string(group) + "_" + std::to_string(index);
    auto owner = motion_toggle_owners_.find(key);
    motion_preview_key_ = owner == motion_toggle_owners_.end() ?
        key : owner->second;
    bool active = selected_motion_keys_.find(motion_preview_key_) !=
        selected_motion_keys_.end();
    std::string playback = motion_to_play(key, &motion_preview_selected_);
    /* A running one-shot is previewed as a cancellation, while a committed
       click keeps the normal replay behavior in start_motion(). */
    if (active && !motion_is_persistent(motion_preview_key_)) {
        motion_preview_selected_ = false;
        playback.clear();
    }
    motion_preview_completed_ = false;
    if (playback.empty())
        return restore_motion_defaults(motion_preview_key_);
    auto found = motions_.find(playback);
    if (found == motions_.end()) {
        restore_motion_preview();
        return false;
    }
    constexpr int priority = 2;
    Csm::CubismMotionQueueEntryHandle handle =
        _motionManager->StartMotionPriority(found->second, false, priority);
    if (handle == Csm::InvalidMotionQueueEntryHandleValue) {
        restore_motion_preview();
        return false;
    }
    record_motion_run(handle, motion_preview_key_,
        motion_preview_selected_, false);
    return true;
}

/* 确认预览: 把这次的勾选态定下来, 并把对应记录标成 committed, 之后按正常播放处理。
 * key 对不上 (用户在预览期间已经切到别的动作) 就直接拒绝 —— 不能拿旧的预览结果去改
 * 新的选择。 */
bool NativeModel::commit_motion_preview(const char *group, int index) {
    if (!motion_preview_active_ || !group || index < 0) return false;
    std::string key = std::string(group) + "_" + std::to_string(index);
    auto owner = motion_toggle_owners_.find(key);
    const std::string &canonical = owner == motion_toggle_owners_.end() ?
        key : owner->second;
    if (canonical != motion_preview_key_) return false;
    select_motion(canonical, motion_preview_selected_);
    for (auto &run : motion_runs_)
        if (!run.committed && run.key == canonical) run.committed = true;
    if (motion_preview_completed_ && !motion_is_persistent(canonical))
        select_motion(canonical, false);
    motion_preview_parameters_.clear();
    motion_preview_parts_.clear();
    motion_preview_key_.clear();
    motion_preview_active_ = false;
    motion_preview_completed_ = false;
    return true;
}

/* 放弃预览: 回滚画面并丢掉快照。返回 false 只表示"本来就没有活跃预览", 是幂等的,
 * 所以 UI 关闭设置窗口时可以无条件调一次。 */
bool NativeModel::restore_motion_preview() {
    if (!motion_preview_active_) return false;
    restore_motion_preview_state();
    motion_preview_parameters_.clear();
    motion_preview_parts_.clear();
    motion_preview_key_.clear();
    motion_preview_active_ = false;
    motion_preview_completed_ = false;
    return true;
}

} // namespace bongo_cat
