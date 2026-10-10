#include "runtime.h"
#include "bongo_cat/memory.h"
#include "bongo_cat/vk_backend.h"
#include "bongo_cat/live2d_vulkan.h"

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

void bongo_cat_memory_policy_model_loaded(void) {
    pending_presented_frames = 2;
}

void bongo_cat_memory_policy_frame_presented(void) {
    consume_pending_frame();
}

void bongo_cat_memory_policy_idle(void) {
    /* Tray-only and minimized modes may not present a frame after loading. */
    consume_pending_frame();
    /*
     * 最小化/托盘: 画面没人看, 把 Live2D 的离屏目标也还掉 —— 超采样后它是窗口的
     * 2 倍大 (十几到几十 MB), 下次绘制时按当前尺寸重建, 只多花一帧。
     */
    bongo_cat_live2d_vulkan_release_target();
}

void bongo_cat_memory_policy_ui_loaded(void) {
    pending_ui_frame = true;
}

void bongo_cat_memory_policy_ui_frame_presented(void) {
    if (!pending_ui_frame) return;
    pending_ui_frame = false;
    trim_memory();
}
