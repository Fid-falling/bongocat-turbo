#include "cubism_model.hpp"

/*
 * 勾选/取消勾选一个动作。
 *
 * "同签名互斥"是这里的关键不变量: 签名相同意味着两个动作碰的是同一组 target:id, 同时
 * 勾选它们没有意义 (后播的会盖掉先播的), 所以勾上新的就把同一签名的旧选择挤掉。
 * 取消勾选只按规范化后的 owner key 删除, 不做连锁清理。
 */
namespace bongo_cat {

void NativeModel::select_motion(const std::string &key, bool selected) {
    auto owner = motion_toggle_owners_.find(key);
    const std::string &canonical = owner == motion_toggle_owners_.end() ?
        key : owner->second;
    if (!selected) { selected_motion_keys_.erase(canonical); return; }
    const std::string &signature = motion_signatures_[canonical];
    /* 空签名 (解析失败/没有可恢复曲线) 不参与互斥: 没有依据说它们互相冲突, 贸然清掉
     * 会误伤用户已经做出的选择。 */
    for (auto item = selected_motion_keys_.begin();
        item != selected_motion_keys_.end();) {
        auto existing = motion_signatures_.find(*item);
        if (!signature.empty() && existing != motion_signatures_.end() &&
            existing->second == signature)
            item = selected_motion_keys_.erase(item);
        else ++item;
    }
    selected_motion_keys_.insert(canonical);
}

} // namespace bongo_cat
