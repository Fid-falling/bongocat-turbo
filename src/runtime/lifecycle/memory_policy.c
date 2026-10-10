/*
 * 内存回收的"时机"策略。真正还什么在 trim_memory() 里:
 *   - 可再生的缓冲 (整帧回读): 随时能放, 下次用到重新分配;
 *   - 空闲堆 + 工作集 (bongo_cat_platform_trim_memory): 归还后进程照常跑。
 * GPU 常驻资源 (纹理/交换链/桌面窗口) 不在这里动 —— 它们要等画面真的没人看才安全。
 *
 * 两个坑:
 *   1. 别把"释放 Live2D 离屏目标"塞进 trim_memory()。模型加载后也会走那条路, 会白
 *      重建一帧 (实测日志里能看到多出来的"已释放离屏目标"); 它只属于最小化/托盘路径。
 *   2. 这些清理是"延迟两帧/一帧"触发的, 因为加载刚完成时那一帧还没画出来, 立刻 trim
 *      会把马上要用的东西放掉。托盘/最小化模式可能永远不出帧, 所以 idle 也要消费一次。
 */

#include "runtime.h"
#include "bongo_cat/memory.h"
#include "bongo_cat/vk_backend.h"
#include "bongo_cat/live2d_vulkan.h"

/* 还要等几个"已呈现帧"才真正回收; 由 model_loaded / ui_loaded 置位, 由 presented /
   idle 消费。 */
static unsigned pending_presented_frames;
static bool pending_ui_frame;

/*
 * 一次内存清理: 先放开"可再生"的大块 (整帧回读缓冲), 再把空闲堆与工作集还给系统。
 * GPU 常驻资源 (纹理、交换链、离屏目标) 不在这里动 —— 它们要等明确的时机
 * (窗口隐藏/最小化) 才能安全释放。
 */
static void trim_memory(void) {
    bongo_cat_vk_release_cached_frames(bongo_cat_vk_current());
    bongo_cat_platform_trim_memory();
}

static void consume_pending_frame(void) {
    if (!pending_presented_frames) return;
    pending_presented_frames--;
    if (!pending_presented_frames) trim_memory();
}

/* 模型加载完成后延迟两帧再回收: 加载那一帧还没画出来, 立刻 trim 会把马上要用的缓冲
   放掉, 下一帧再白分配一次。 */
void bongo_cat_memory_policy_model_loaded(void) {
    pending_presented_frames = 2;
}

/* 渲染路径在"帧真的呈现之后"调用 (不是画完就算): 缓冲可能马上要被复用, 呈现完才是
   安全点。 */
void bongo_cat_memory_policy_frame_presented(void) {
    consume_pending_frame();
}

/* 窗口不可见 / 最小化时主循环每轮都会调用它, 所以这里是"没人看画面"时的兜底消费点
   —— 托盘模式可能永远不出帧, 只靠 presented 消费的话永远等不到。 */
void bongo_cat_memory_policy_idle(void) {
    /* Tray-only and minimized modes may not present a frame after loading. */
    consume_pending_frame();
    /*
     * 最小化/托盘: 画面没人看, 把 Live2D 的离屏目标也还掉 —— 超采样后它是窗口的
     * 2 倍大 (十几到几十 MB), 下次绘制时按当前尺寸重建, 只多花一帧。
     */
    bongo_cat_live2d_vulkan_release_target();
}

/* 设置窗口的 UI 纹理同理: 打开后延迟一帧回收 (UI 缓存自己另有一套 use_counter 机制)。 */
void bongo_cat_memory_policy_ui_loaded(void) {
    pending_ui_frame = true;
}

void bongo_cat_memory_policy_ui_frame_presented(void) {
    if (!pending_ui_frame) return;
    pending_ui_frame = false;
    trim_memory();
}
