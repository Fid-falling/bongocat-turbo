# Vulkan 渲染后端

`G:\bongocat-vulkan` 在 BongoCat 之上新增的独立渲染后端。它和原有的 OpenGL 后端
并存，由设置里的「渲染后端」决定这次运行走哪条路；两条路不共享任何 GPU 资源。

## 怎么用

```powershell
# 配置阶段必须打开 Vulkan 开关，SDL 的 Vulkan 支持与 vulkan-1 链接才会启用
cmake -S . -B build-vulkan -A x64 -DBONGO_CAT_ENABLE_VULKAN=ON
cmake --build build-vulkan --config Release --target bongo_cat --parallel 8
```

存储目录的 `config/settings.json`：

```json
{ "rendering": { "renderer": "vulkan" } }
```

`renderer` 取 `auto` / `opengl` / `vulkan`。`auto` 目前仍解析成 OpenGL；选 `vulkan`
时如果探测不到可用设备，或初始化中途失败，运行时会把原因写进日志并回退 OpenGL，
不会把窗口留在半初始化状态。

有用的环境变量：

| 变量 | 作用 |
| --- | --- |
| `BONGO_CAT_VULKAN_VALIDATION=1` | 打开 `VK_LAYER_KHRONOS_validation` 并把消息转发到日志 |
| `BONGO_CAT_TEST_VULKAN_FALLBACK=1` | 强制 Vulkan 初始化失败，用于验证回退路径 |

## 结构

| 文件 | 职责 |
| --- | --- |
| `include/bongo_cat/vk_backend.h` | 对外接口：上下文、帧、纹理、绘制、回读 |
| `src/render/vk/vk_core.c` | 实例、设备选择、队列、生命周期 |
| `src/render/vk/vk_swapchain.c` | 交换链、渲染通道、帧缓冲、呈现信号量 |
| `src/render/vk/vk_frame.c` | 帧同步、录制、提交呈现、命中回读 |
| `src/render/vk/vk_texture.c` | 纹理上传 (暂存缓冲 → 图像)、描述符、延迟释放 |
| `src/render/vk/vk_draw.c` | 管线、顶点缓冲、叠加层/指针/圆角遮罩绘制 |
| `src/render/vk/vk_scene.c` | 主窗口一帧的绘制顺序 |
| `src/render/vk/shaders/*` | GLSL，配置阶段由 `glslc` 编成 SPIR-V 并嵌入 C 头 |
| `src/render/render_texture.h` | 后端无关纹理句柄 (`BongoCatTexture`) |

## 与 OpenGL 路径的对应关系

| OpenGL | Vulkan |
| --- | --- |
| `glClearColor` + `glClear` | 渲染通道的 `loadOp = CLEAR` |
| 叠加层四边形着色器 (`overlay.c`) | `shaders/overlay.vert|frag`，同一套镜像/参考尺寸/脚印擦除语义 |
| 指针顶点色图元 (`mver_pointer_overlay_draw.c`) | `shaders/pointer.*`，三角形扇/带在 CPU 展开 |
| 圆角遮罩 (`window_background.c`) | `shaders/corner.*`，`GL_ZERO / GL_SRC_ALPHA` 对应 `ZERO / SRC_ALPHA` |
| `glDrawArrays` + `blend_func_separate(ONE, ONE_MINUS_SRC_ALPHA)` | 预乘 alpha 的固定混合状态 |
| `glReadPixels` 命中测试 | 交换链图像 → 主机缓冲的小块回读 (延迟一帧) |

坐标约定：顶点数据仍按 OpenGL 的约定生成，顶点着色器末尾对 y 取反以适配 Vulkan
裁剪空间；纹理第 0 行是图像顶部。

## 已知边界

- **Cubism 模型**：Live2D 的渲染器仍是 OpenGL 实现，因此带 Cubism 的构建里
  Vulkan 会被标记为不可用并回退 OpenGL，而不是画出一个没有模型的窗口。
- **模型封面截取 / 窗口快照**：需要离屏渲染 + 回读，尚未接入 Vulkan；这两条路径
  在 Vulkan 下会延后处理。
- **`auto`**：等上面两项补齐后再让它优先选 Vulkan。
