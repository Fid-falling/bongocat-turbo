#include "runtime.h"
#include "bongo_cat/memory.h"
#include "bongo_cat/vk_backend.h"

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
}

void bongo_cat_memory_policy_ui_loaded(void) {
    pending_ui_frame = true;
}

void bongo_cat_memory_policy_ui_frame_presented(void) {
    if (!pending_ui_frame) return;
    pending_ui_frame = false;
    trim_memory();
}
