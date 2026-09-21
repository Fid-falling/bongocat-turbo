# Vulkan 状态更新（第 75 轮）— 透明合成、设置窗口与“仅录屏可见”

`docs/vulkan-status-update-r72.md` 的后续。本轮修掉用户报告的两个可见缺陷，另外
加了一个新功能。

## 一、Vulkan 主窗口背景是黑的（用户报告）

用户报告：用 Vulkan 渲染时桌面宠物的背景是黑的，应该是透明的。

Windows 上 SDL 用 `DwmEnableBlurBehindWindow`（空模糊区域）让 DWM 按窗口重定向
表面的 alpha 逐像素合成，所以“窗口透不透明”取决于帧缓冲的 alpha 能不能送到
合成器。OpenGL 的 GDI 表面天然带 alpha；Vulkan 的交换链以前一律按
`compositeAlpha=OPAQUE` 创建，DWM 拿到的 alpha 全是 1，于是 `glClear` 的
`(0,0,0,0)` 被合成为一个黑色方块。

修法（`src/render/vk/vk_swapchain.c`）：窗口带 `SDL_WINDOW_TRANSPARENT` 时首选
`VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR`（其次 POST_MULTIPLIED / INHERIT /
OPAQUE），驱动报支持却拒绝创建时退回 OPAQUE 并记警告；交换链日志补上
`composite_alpha / supported / transparent` 三个字段，测试可以直接断言。

| | 交换链合成 | 桌面观感 |
|---|---|---|
| OpenGL | GDI 表面自带 alpha | 透明 |
| Vulkan（修前） | `composite_alpha=0x1`（OPAQUE） | **黑底** |
| Vulkan（修后） | `composite_alpha=0x2`（PRE_MULTIPLIED），`supported=0x3` | 透明 |

实测：`build-vulkan/.test/vk-alpha.png` 里窗口区域能看到后面的桌面内容；无限帧率
模式下 `present_mode=0`（IMMEDIATE）与预乘合成共存，20 s 内 15475 帧（约 823 FPS）。

## 二、设置窗口在 Vulkan 下必崩

设置窗口以前自己建一套 `VkInstance/VkDevice/队列`。本机常驻的注入式图形钩子
（RTSS / OBS / NVIDIA 叠加层）假定一个进程只有一套设备，第二个上下文一到
`begin_frame` 就踩坏驱动状态：日志停在 `[prefs] stage=present-begin`，进程
`0xC0000005`，解析出的栈是 `bongo_cat_vk_end_frame+0x173`。

修法：新增 `bongo_cat_vk_create_shared()`，设置窗口复用主窗口的
instance/physical/device/queue，只建自己的表面、交换链与设备级资源；用
`owns_device` 标记借来的设备；销毁路径按它分流。设置窗口重新跟随渲染后端
（`BONGO_CAT_PREFERENCES_OPENGL=1` 可强制退回 GL）。

实测：设置窗口 `shared=1`，`[prefs] stage=present-done` 连续循环 39 s 无崩溃
（修前 0.5 s 内必崩）。

## 三、新功能：仅在录屏软件中显示（`captureOnly`）

需求：桌面宠物只在 OBS 之类的录屏软件里出现，桌面上不显示。

实现：用 DWM 隐藏（`DWMWA_CLOAK`）把主窗口从桌面合成里摘掉。窗口照常渲染、
照常呈现，游戏采集直接读交换链/GL 呈现、窗口采集读窗口自己的内容，所以采集侧
画面不变，桌面侧完全看不到。设置界面“窗口”页新增开关（仅 Windows 显示），
`settings.json` 里是 `captureOnly`，10 种语言都补了文案。

因为窗口在桌面上不可见，点击穿透必须整体生效——但这里特意**不走** forced 分支：
那条路会启用 layered presenter，用代理窗口把画面重新显示回桌面，正好和
“桌面不显示”冲突。

验证（`build-vulkan/.test/capture-only-probe.ps1`，clean1 存储）：
`captureOnly=1` 时窗口区域截屏只有桌面、`=0` 时猫正常显示；`WindowFromPoint`
（窗口中心）命中其它进程，说明点击穿透生效；渲染没有中断（10 s 内 13518 帧）；
日志 `[runtime] capture-only=1` 可断言。Vulkan 与 OpenGL 两条路径都生效。

## 五、默认渲染后端改回 OpenGL

Vulkan 后端现在能跑通主窗口、设置窗口和 Live2D，但还没到"可以当默认"的程度
（设置界面里的图片贴图仍是纯色方块，采集路径也有差异）。所以：

- `AUTO` 解析为 **OpenGL**（`src/render/render_backend.c` 的 `resolve`），
  Vulkan 需要用户在设置里显式选择；
- 新配置的默认值写成 `"renderer": "opengl"`（`config_defaults.c`）；
- 旧配置里的 `"auto"` 会跟着落到 OpenGL，选过 Vulkan 的配置不受影响。

实测：`renderer=auto → 实际使用=opengl`、`=vulkan → vulkan`、`=opengl → opengl`。
等 Vulkan 追平之后，把 `resolve` 里那三行改回"优先 Vulkan"即可整体切回去。

## 六、鼠标穿透不再藏起真实窗口

用户报告"开启鼠标穿透后 OBS 的捕获就卡住了"。原因是穿透模式会启用
**layered presenter**：把真实窗口压成完全透明（`SetLayeredWindowAttributes`
alpha=0），再用一个代理窗口（逐帧 `glReadPixels` + `UpdateLayeredWindow`）把画面
重新显示出来。OBS 里采集的是那个真实窗口，一开穿透它就全透明，画面自然"卡住"；
Vulkan 下更是连 GL 前缓冲都没有，代理永远拿不到新帧。

现在穿透只设置 `WS_EX_TRANSPARENT`，真实窗口留在正常合成路径上（它本身就能被
游戏采集/窗口采集抓到）。实测 OpenGL/Vulkan 两条路径：开穿透后桌面上只剩一个
`ex=0x28`（WS_EX_TRANSPARENT|TOPMOST）的可见窗口，没有 `WS_EX_LAYERED`、没有
代理窗口，宠物照常动（3 s 帧差 2.16% / 1.34%）。

## 七、单独复制 exe 导致 Vulkan 画坏（用户报告）

用户报告：把 `BongoCat.exe` 单独复制到桌面后打开，Vulkan 下渲染不正常。

原因：Cubism 的 Vulkan 渲染器按**相对路径** `FrameworkShaders/*.spv` 打开 510 个
编译好的着色器，它们本来是随发布包放在 exe 旁边的。只复制一个 exe 时这些文件
就没了，模型管线建不起来，画面上只剩一小块破碎的图形。

修法（两层）：

1. 这些 `.spv` 现在也打进内嵌资源（`assets/FrameworkShaders/`）。桥在挂载时按
   "exe 目录 → 解出来的内嵌资源目录"的顺序找着色器，并 `chdir` 到它的父目录
   （SDK 用的是相对路径）。所以单独一个 exe 也能跑 Vulkan —— 实测：把 exe 复制
   到别的目录，日志里 `着色器目录=...\cache\embedded-assets-<ver>\assets`，
   Vulkan 正常出画。代价是 exe 从 6.2 MB 变成 9.6 MB。
2. 两处都找不到时**明确失败并回退 OpenGL**，而不是画出一堆碎片：日志会写
   `vulkan 初始化失败 (缺少 Live2D 的 Vulkan 着色器 (FrameworkShaders/*.spv): ...)`。

## 八、验证工具

- `build-vulkan/.test/alpha-screen.ps1`：等日志标记再截窗口（本机启动有时很慢，
  不能靠 `MainWindowHandle` 轮询）；
- `build-vulkan/.test/capture-only-probe.ps1`：`captureOnly` 的 A/B 截屏 +
  `WindowFromPoint` 穿透检查；
- `build-vulkan/.test/passthrough-probe.ps1`：开穿透后枚举进程的顶层窗口
  （看有没有被压成全透明的源窗口和代理窗口）+ 两次截屏算帧差（冻结帧差≈0）。

## 九、已知遗留

- 设置界面里的图片贴图（logo、侧边栏图标、关闭按钮）在 Vulkan 下仍画成纯色
  方块（paint cache 那几张纹理采样不到内容），字体图集不受影响；
- 构建环境在本次会话中途被 C 盘格式化清空，恢复过程见
  `docs/build-environment-restore.md`；
- 覆盖截帧 / 整帧回读仍只有 OpenGL 路径（`docs/vulkan-readback-recipe.md`）；
- `live2d-visual-audit` 的 `edge_pixels` 列依赖 `glReadPixels`，Vulkan 下无意义。
