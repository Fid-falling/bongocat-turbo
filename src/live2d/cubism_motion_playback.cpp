/*
 * 动作播放的记账 (MotionRun)。
 *
 * SDK 的 CubismMotionManager 只知道"哪个动作还在播", 不知道它属于哪个 UI 条目、是勾选型
 * 还是一次性、是否已经被用户确认。这些信息全在 motion_runs_ 里: 每次开始播放登记一条,
 * 播完时再由 expire_motion_runs() 决定要不要清掉 UI 勾选、要不要把参数恢复成原值。
 * 这里是"UI 状态"与"SDK 播放状态"之间唯一的对齐点, 改动它要同时看 UI 侧怎么读勾选态。
 */
#include "cubism_model.hpp"

#include <Motion/CubismMotionQueueEntry.hpp>
#include <Motion/CubismMotionManager.hpp>
#include <utility>

namespace bongo_cat {

bool NativeModel::start_motion(const char *group, int index) {
    if (!group || index < 0) return false;
    std::string key = std::string(group) + "_" + std::to_string(index);
    bool selected = false;
    std::string playback = motion_to_play(key, &selected);
    if (playback.empty()) {
        auto owner = motion_toggle_owners_.find(key);
        const std::string &canonical = owner == motion_toggle_owners_.end() ?
            key : owner->second;
        if (!restore_motion_defaults(canonical)) return false;
        select_motion(canonical, false);
        return true;
    }
    auto found = motions_.find(playback);
    if (found == motions_.end()) return false;
    constexpr int priority = 2;
    Csm::CubismMotionQueueEntryHandle handle =
        _motionManager->StartMotionPriority(found->second, false, priority);
    if (handle == Csm::InvalidMotionQueueEntryHandleValue) return false;
    select_motion(key, selected);
    record_motion_run(handle, key, selected, true);
    return true;
}

/* key 先规范化成 owner: 一对开关共用一条记录, 免得两侧各记一份、互相矛盾。
 * one_shot 反过来推 —— "不是持久型"就是一次性。committed 区分"用户点的"与"预览的":
 * 预览产生的记录不能影响 UI 勾选。 */
void NativeModel::record_motion_run(Csm::CubismMotionQueueEntryHandle handle,
    const std::string &key, bool selected, bool committed) {
    auto owner = motion_toggle_owners_.find(key);
    const std::string &canonical = owner == motion_toggle_owners_.end() ?
        key : owner->second;
    MotionRun run;
    run.handle = handle;
    run.key = canonical;
    run.one_shot = !motion_is_persistent(canonical);
    run.selected = selected;
    run.committed = committed;
    motion_runs_.push_back(std::move(run));
}

void NativeModel::stop_motion_runs(const std::string &key) {
    for (const MotionRun &run : motion_runs_) {
        if (run.key != key) continue;
        auto *entry = _motionManager->GetCubismMotionQueueEntry(run.handle);
        if (entry) entry->IsFinished(true);
    }
}

/*
 * 每帧调用, 处理已经播完的动作。三类记录分开处理:
 *  1) 未提交的 (预览) 记录: 只打个完成标记, 交给 UI 决定是 commit 还是 restore;
 *  2) 持久型动作播完: 把曲线写回终态值, 让它停在"开"的位置 —— 这是开关型动作的语义,
 *     用户勾上以后即使不播了, 模型也该保持那个姿势;
 *  3) 一次性动作播完: 先看有没有同签名的后继在播, 没有才清勾选。
 * 最后原地压缩数组, 把已完成且清理干净的记录挤出去。
 */
void NativeModel::expire_motion_runs() {
    if (motion_runs_.empty()) return;
    /* finished[] 先整体采样再统一处理: 处理过程中会调 select_motion/apply_motion_curve
     * 改动模型与队列状态, 边查边改的话后半段问到的就未必还是同一帧的答案了。 */
    auto &finished = motion_finished_scratch_;
    finished.resize(motion_runs_.size());
    for (size_t i = 0; i < motion_runs_.size(); ++i)
        finished[i] = _motionManager->IsFinished(motion_runs_[i].handle);
    bool restored_persistent_state = false;
    for (size_t i = 0; i < motion_runs_.size(); ++i) {
        const MotionRun &run = motion_runs_[i];
        if (!finished[i]) continue;
        if (!run.committed) {
            if (motion_preview_active_ && run.key == motion_preview_key_)
                motion_preview_completed_ = true;
            continue;
        }
        if (!run.one_shot) {
            bool currently_selected = selected_motion_keys_.find(run.key) !=
                selected_motion_keys_.end();
            if (currently_selected == run.selected) {
                auto state = motion_states_.find(run.key);
                if (state != motion_states_.end())
                    for (const auto &curve : state->second.curves)
                        restored_persistent_state = apply_motion_curve(curve,
                            run.selected ? curve.end : curve.normal) ||
                            restored_persistent_state;
            }
        }
        bool replacement_running = false;
        if (run.one_shot) {
            auto source_signature = motion_signatures_.find(run.key);
            for (size_t j = 0; j < motion_runs_.size(); ++j) {
                auto replacement_signature = motion_signatures_.find(
                    motion_runs_[j].key);
                if (j != i && !finished[j] && motion_runs_[j].committed &&
                    (motion_runs_[j].key == run.key ||
                    (source_signature != motion_signatures_.end() &&
                    !source_signature->second.empty() &&
                    replacement_signature != motion_signatures_.end() &&
                    source_signature->second == replacement_signature->second))) {
                    replacement_running = true;
                    break;
                }
            }
        }
        if (motion_run_clears_selection(run.one_shot, run.committed,
            replacement_running)) select_motion(run.key, false);
    }
    if (restored_persistent_state) {
        save_parameters();
    }
    size_t output = 0;
    for (size_t i = 0; i < motion_runs_.size(); ++i)
        if (!finished[i]) motion_runs_[output++] = std::move(motion_runs_[i]);
    motion_runs_.resize(output);
}

/* 清空记账 (换模型/重载) 时, 顺手把仍然"勾着"的一次性动作取消勾选, 免得新模型上残留
 * 上一份模型的选择。持久型动作不动 —— 它们的状态本来就要跟着模型走。 */
void NativeModel::clear_motion_runs() {
    for (const MotionRun &run : motion_runs_)
        if (run.committed && run.one_shot) select_motion(run.key, false);
    motion_runs_.clear();
}

bool NativeModel::motion_selected(const char *group, int index) const {
    if (!group || index < 0) return false;
    std::string key = std::string(group) + "_" + std::to_string(index);
    auto owner = motion_toggle_owners_.find(key);
    if (owner != motion_toggle_owners_.end()) key = owner->second;
    return selected_motion_keys_.find(key) != selected_motion_keys_.end();
}

/* 配对项 (partner) 不在 UI 里单独列出, 由 owner 代表整对, 所以只有 owner 返回 true。 */
bool NativeModel::motion_visible(const char *group, int index) const {
    if (!group || index < 0) return false;
    std::string key = std::string(group) + "_" + std::to_string(index);
    auto owner = motion_toggle_owners_.find(key);
    return owner == motion_toggle_owners_.end() || owner->second == key;
}

bool NativeModel::motion_same_toggle(const char *left_group, int left_index,
    const char *right_group, int right_index) const {
    if (!left_group || !right_group || left_index < 0 || right_index < 0)
        return false;
    std::string left = std::string(left_group) + "_" +
        std::to_string(left_index);
    std::string right = std::string(right_group) + "_" +
        std::to_string(right_index);
    auto left_owner = motion_toggle_owners_.find(left);
    auto right_owner = motion_toggle_owners_.find(right);
    if (left_owner == motion_toggle_owners_.end() ||
        right_owner == motion_toggle_owners_.end()) return false;
    return left_owner->second == right_owner->second;
}

} // namespace bongo_cat
