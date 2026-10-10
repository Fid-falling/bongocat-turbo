#include "cubism_model.hpp"

#include <Motion/CubismMotionManager.hpp>
#include <cstdlib>

namespace bongo_cat {

/*
 * 随机挑一个 Idle 动作, 但避开上一次那个 —— 连着播同一个待机动作看起来像卡住了。
 * 优先级 1 低于用户动作的 2, 所以待机动作不会抢掉正在播的点击动作; ReserveMotion 失败
 * 说明队列里还有更高优先级的动作没播完, 这一帧就放弃。
 */
void NativeModel::start_idle_motion() {
    if (idle_motion_keys_.empty()) return;
    constexpr int priority = 1;
    if (!_motionManager->ReserveMotion(priority)) return;
    int next = std::rand() % (int)idle_motion_keys_.size();
    if (next == last_idle_motion_)
        next = (next + 1) % (int)idle_motion_keys_.size();
    last_idle_motion_ = next;
    const std::string &key = idle_motion_keys_[(size_t)next];
    auto found = motions_.find(key);
    if (found != motions_.end())
        _motionManager->StartMotionPriority(found->second, false, priority);
}

} // namespace bongo_cat
