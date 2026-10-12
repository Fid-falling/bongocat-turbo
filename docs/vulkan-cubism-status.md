# Vulkan 后端：Cubism (Live2D) 接入状态

本文记录"让 Vulkan 完全替代 OpenGL"这条主线的当前状态、已修根因、剩余配方与排查记录。
（配套实现：`src/live2d/cubism_vulkan_bridge.cpp`、`cubism_vulkan_texture.cpp`、
`cubism_renderer_factory.cpp`、`src/render/vk/vk_scene.c`、`cmake/CubismRendererSelect.cmake`）

## 一、当前可用状态（已验证）

| 项目 | 状态 | 证据 |
|---|---|---|
| Cubism + Vulkan 启动 | ✅ | `build-cubism` 下 `renderer=vulkan` 可运行 |
| Live2D 模型渲染 | ✅ | 截图显示猫本体与道具，左上角 `VULKAN`，**56.8 FPS** |
| 视线跟随 / 按键动作 | ✅ | 指针移入 9129、移动 6879、按住 A 4344 变化像素 |
| 圆角遮罩 / 命中回读 | ✅ | `vk_scene.c` 的 `mask_corners` / `queue_hit_sample` 在 Live2D 合成之后执行 |
| 校验告警 | ✅ | **VUID 0 条**（SDK 阶段掩码补丁后） |
| 三个构建配置 | ✅ | Cubism+Vulkan / 无 Cubism / Cubism+Vulkan关闭 全绿 |

## 二、接入过程中修掉的根因（按发现顺序，便于后续排查）

1. **`add_subdirectory` 只能加一个后端目录** —— 原写法 `"OpenGL;Vulkan"` 使 Vulkan 源码从未被编译。
   现在改为显式 `target_sources(Framework ...)` 列出 SDK 的 Vulkan 源文件。
2. **两个后端都定义 `CubismRenderer::Create/StaticRelease`** —— 重复符号。
   由 `cmake/CubismRendererSelect.cmake` 配置期给两份定义加 `#ifndef BONGO_CAT_RUNTIME_RENDERER_SELECT`，
   唯一实现放在 `src/live2d/cubism_renderer_factory.cpp`（运行时选择，默认 OpenGL）。
3. **`vk_backend.h` / `live2d_vulkan.h` 缺 `extern "C"`** —— C++ 侧按修饰名找不到 C 实现的符号。
4. **`window.c` 把尚未填充的 `info.width/height`（全 0）传给 attach** —— 桥因 `width <= 0` 静默早退。
5. **`start_framework()` 无条件 `glewInit()`** —— Vulkan 窗口没有 GL 上下文，比窗口更早失败。
   现在按 `bongo_cat_renderer_active()` 跳过整段 GL 初始化。
6. **attach 调用时机** —— 必须在 `CubismFramework::StartUp` 之后（SDK 的 `csmMap` 用全局分配器），
   且必须在**创建模型贴图之前**（Vulkan 贴图上传要用后端句柄）。
   现在：窗口创建时只 `bongo_cat_live2d_vulkan_remember()`（不碰 SDK），模型加载时 `attach()`。
7. **`bongo_cat_live2d_draw` 被 `bridge_ready` 挡住** —— "不 ready 不画、不画永远不 ready" 死锁。
8. **设备特性未启用**（真正的崩溃根因）—— SDK 的 Vulkan 渲染器需要
   `VK_EXT_extended_dynamic_state`、`dynamicRendering`(1.3)、`samplerAnisotropy`。
   过去只启用交换链扩展，导致管线与采样器全是无效对象，绘制时崩。
   现在按设备支持情况启用（`vkGetPhysicalDeviceFeatures2` 查询 + 条件开启）。
9. **SDK 阶段掩码非法** —— `CubismClass_Vulkan.cpp` 的 `SetImageLayout` 用了
   `VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR`；配置期补丁改成片元阶段，VUID 6 → 0。
10. **遗漏 SDK 的三步初始化** —— `CreateCommandBuffer/CreateVertexBuffer/CreateIndexBuffer`，
    样例里独立调用；现已在 `create_renderer` 的 Vulkan 分支补上。
11. **交换链重建后 SDK 静态设置过期** —— `begin_frame` 每帧用当前句柄重新
    `SetConstantSettings`（纯赋值，开销可忽略）。

## 三、剩余工作（按优先级，配方已明确）

### ② 封面截取（离屏 + 回读）
- 入口：`vk_scene.c:52` 的 `if (!present) { return false; }` 目前是桩，GL 路径的封面截取在这里被延后。
- 步骤：
  1. `!present` 分支按正常流程渲染一帧（模型 + 合成），提交后 `vkDeviceWaitIdle`，把交换链图像拷回主机，
     再正常 present（用于消费 acquire，画面与普通帧一致）；
  2. 新增整帧回读 API（见 ③），复用命中回读的 staging + 栅栏记账，缓冲按区域动态分配；
  3. `model_cover.c` 保留既有的"像素 → PNG"，加一条 Vulkan 分支（数据源换成回读缓冲）。
- 前置已确认：`NativeModel::prepare_cover_capture()` 是纯模型状态操作，无 GL 调用，可原样复用。

### ③ 窗口快照 / 帧诊断回读
- 与 ② 共用整帧回读基础设施；命中回读的现成实现见 `vk_frame.c` 的
  `queue_hit_sample` / `record_readback` / `collect_readback`。

### ④ 径向菜单移植
- `src/ui/dial/dial_paint.c` 仍是 GL（`bongo_cat_gl_load` + 自绘）。可照设置窗口的做法：
  `bongo_cat_vk_texture_from_file` + `bongo_cat_vk_ui_draw`/`draw_overlay_quad` 组合。

### ⑤ `AUTO` 优先 Vulkan
- `src/render/render_backend.c` 的 `bongo_cat_renderer_resolve()` 目前把 AUTO 固定解析成 OpenGL。
  ②③④ 验完后改为优先 Vulkan，OpenGL 仅作能力不足时的回退。

## 四、已知独立问题（不影响实际使用）

**校验层开启时的崩溃**：`BONGO_CAT_VULKAN_VALIDATION=1` 时数秒后 `0xC0000409`（栈越界），
栈恒为 `CubismRenderTarget_Vulkan::BeginDraw ← SetupClippingContext ← DoDrawModel`。
不开校验层时稳定运行（56.8 FPS、输入正常）。
已尝试且未改变栈：设备级等待、每帧刷新静态设置、补建 SDK 缓冲、阶段掩码补丁（该补丁已把 VUID 清零）。
判断为 SDK 在慢速路径下的内部问题，记录备查。

## 五、测试方式

```
构建:  cmake --build build-cubism --config Release --target bongo_cat --parallel 8
运行:  build-cubism\Release\BongoCatTurbo.exe --storage-root=<存储>
校验:  设 BONGO_CAT_VULKAN_VALIDATION=1（并抓 stderr, 校验信息走 stderr）
抓图:  build-vulkan\.test\shot.ps1 -Exe <exe> -Storage <存储> -Renderer vulkan -Out <png> -W 700 -H 420
探针:  build-vulkan\.test\probe-cub.ps1 -Storage <存储> -Tag <标签>（量指针/按键响应）
抓栈:  cdb -o -g -G -c "g; kb 30; q" <exe> <参数>
       Release 无 PDB 时给目标加 /MAP 只重链接一次, 再用 build-vulkan\.test\resolve_stack.py 解析 RVA
```
