# 整帧回读实现配方（封面截取 / 窗口快照共用）

面向"让 Vulkan 完全替代 OpenGL"的后续工作。现状与总览见 `docs/vulkan-cubism-status.md`。

## 现有回读设施（已勘察）

`src/render/vk/vk_internal.h`：

```c
typedef struct BongoCatVkReadback {
    VkBuffer buffer;  VkDeviceMemory memory;  unsigned char *mapped;
    bool requested;   /* 这一帧要录制拷贝 */
    bool pending;     /* 已录制, 等下次复用该帧槽时读取 */
    int origin_x, origin_y, width, height;   /* 回读区域 (目前是命中点小块) */
} BongoCatVkReadback;
```

`src/render/vk/vk_frame.c`：

- `record_readback()`：交换链图像 `PRESENT_SRC → TRANSFER_SRC` → `vkCmdCopyImageToBuffer(区域)` → 转回 `PRESENT_SRC`；
- `collect_readback()`：下一帧复用该帧槽时，把映射内存里**每像素第 4 字节（alpha）**写进 `vk->hit_alpha`，
  并记录 `hit_x/y/width/height`；
- 每帧槽一个 staging buffer，容量在创建时按命中区域固定分配。

## 要做的改动（约 120 行，4 个文件 + 桩）

1. **`vk_internal.h`**
   - `BongoCatVkReadback` 加 `bool frame;`
   - `BongoCatVk` 加整帧结果缓冲：`unsigned char *frame_pixels; int frame_width; int frame_height;`（堆分配）
2. **`vk_frame.c`**
   - `collect_readback()` 开头分两支：`frame` 为真时按 **RGBA 四字节**拷进 `vk->frame_pixels`（必要时 realloc），
     否则保持现有 alpha 路径；
   - `record_readback()` 不必改（它按 `readback->width/height` 工作）。
3. **staging buffer 容量**
   - 命中回读是固定小块；整帧需要在入队时按 `w*h*4` 重新创建该帧槽的 buffer，
     建缓冲/找内存类型的写法可参考 `src/render/vk/vk_draw.c` 里 arena 的创建。
4. **新 C API**（声明进 `include/bongo_cat/vk_backend.h`，桩进 `src/render/vk/vk_stub.c`）

   ```c
   void bongo_cat_vk_queue_frame_sample(BongoCatVk *vk, int x, int y, int w, int h);
   bool bongo_cat_vk_take_frame_sample(BongoCatVk *vk, int *w, int *h,
       const unsigned char **pixels);   /* 与命中回读同为"一帧延迟"语义 */
   ```
5. **封面截取接线**
   - `src/render/vk/vk_scene.c` 里 `if (!present) { return false; }`（当前是桩）改为：
     渲染这一帧 → 入队整帧回读 → 正常 present（用于消费 acquire，画面与普通帧一致）；
   - `src/live2d/../model_cover.c` 保留现成的"像素 → PNG"，加一条 Vulkan 分支，
     数据源换成回读缓冲（`glReadPixels` 只在 GL 后端下走）；
   - 前置已确认：`NativeModel::prepare_cover_capture()` 是纯模型状态操作，无 GL 调用，可原样复用。
6. **窗口快照 / 帧诊断回读（③）**
   - 与 ② 共用上面 1–4 的基础设施，只是调用方不同（诊断时把整帧回读结果落盘/写日志）。
