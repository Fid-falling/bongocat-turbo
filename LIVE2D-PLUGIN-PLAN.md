# Live2D 插件化 + 用户自备 Cubism Core —— 落地方案（只读调研）

> 本文件是**调研 + 方案**，不含任何代码改动。写这份文件的过程只做了只读操作：
> 读 `TianMengLucky/BongoCat-X` 的克隆、读本仓库文件、跑 `git log`/`git grep`。
> 没有 `git add`、没有提交、没有发 release、没有启动任何进程、没有碰 `%LOCALAPPDATA%\BongoCat`，
> 也没有接触正在运行的实例（PID 50532）。

| 项 | 值 |
| --- | --- |
| 调研对象 | `TianMengLucky/BongoCat-X`，克隆位于 `%TEMP%\bcatx\BongoCat-X`，分支 `dev`，HEAD `0d0429defa0876c33bd7bfc0d0cd71bba16b02d7`（2026-10-14，`perf: trim render memory and remove the low-memory mode toggle`） |
| 我方仓库 | `G:\bongocat-vulkan`，分支 `main`，HEAD `5d126a3`（`fix(vulkan): 修掉 Live2D 半透明边缘再乘一次 alpha…`） |
| 工作树状态（如实记录） | 本次调研开始时 `git status` 干净。调研过程中 **00:06:25-00:07:02** 有**另一个操作者**（共享同一工作区，非本次调研）改动了 11 个与本主题无关的文件（`include/bongo_cat/{app.h,config.h}`、`src/core/config_*.c`、`src/runtime/lifecycle/{app.c,frame_clock.c}`、`src/ui/preferences/{preferences_icons.h,preferences_pages.c,preferences_row_icons.c}`、`resources/assets/locales/en-US.json`，合计 +109/-9）。**本报告没有修改、也没有回退它们**；本报告唯一的文件写入是新建本文件（`LIVE2D-PLUGIN-PLAN.md`，未 `git add`） |
| 本文件 | `G:\bongocat-vulkan\LIVE2D-PLUGIN-PLAN.md`（新建，未纳入 git） |
| 前一轮已确认、本文件直接采用 | Core 是预编译二进制（可 dlopen）；Framework 是必须现场编译的源码树；因此可行路线 = 随包发**已编译插件** + 用户自备 **Core**；代价 = 不再是单文件 exe |

---

## 目录

1. [结论速览](#1-结论速览)
2. [X 的许可 / 同意流程（原文 + commit）](#2-x-的许可--同意流程)
3. [X 的构建期守卫](#3-x-的构建期守卫)
4. [X 的 CI / 包体检查](#4-x-的-ci--包体检查)
5. [X 缺 Core / 缺插件时的降级路径](#5-x-缺-core--缺插件时的降级路径)
6. [X 的插件 ABI 六件套（我们的参考设计）](#6-x-的插件-abi-六件套)
7. [X 的弱点 —— 哪些不要照抄](#7-x-的弱点哪些不要照抄)
8. [我们的现状（为什么非改不可）](#8-我们的现状为什么非改不可)
9. [我们的落地设计](#9-我们的落地设计)
10. [改动面清单](#10-改动面清单)
11. [工作量估计（乐观 / 保守）](#11-工作量估计乐观--保守)
12. [对用户的影响 & 发布说明怎么写](#12-对用户的影响--发布说明怎么写)
13. [风险](#13-风险)
14. [替代方案排序](#14-替代方案排序)
15. [附录 A：证据索引（commit + 文件）](#附录-a证据索引)
16. [附录 B：未确认项 / 不要照抄清单](#附录-b未确认项--不要照抄清单)

---

## 1. 结论速览

1. **X 的边界正好是"我们发插件、用户供 Core"**：`cmake/Cubism.cmake:34` 原文 `# Core is never linked into the plugin. Only its API header is needed here.`；宿主 exe 与插件都不链接 Core，Core 的 `csm*` ABI 由**配置期生成的 shim** 持有，运行期通过宿主服务表 `core_symbol` 转发（`cmake/gen_core_shim.py:2-14`、`include/bongo_cat/model_plugin_host.h:21`）。
2. **X 的同意流程是"先读两份官方协议、用户点同意、程序才下载官方 SDK zip、只取匹配架构的动态 Core"**（`src/ui/preferences/preferences_live2d_setup.c:24-47,108-119`、`src/rust/bongo-safe/src/live2d_download.rs:12,53-56`）；另有"手动导入 Core / 把 zip 丢进 `live2d/` 文件夹"两条不联网路径（`README.md:54-57`）。
3. **X 的构建期守卫有两条硬的**：`CMakeLists.txt:11-16`（`BONGO_CAT_RUNTIME_CORE=OFF` 直接 `FATAL_ERROR: renderer plugins must not embed Cubism Core`）与 `cmake/Cubism.cmake:34` 的注释纪律；`cmake/ModelPlugins.cmake:2` 明确 "the host never links Framework"。
4. **X 的 CI 有两处"发行包不含 Core"的断言**：`.github/scripts/check-unix-package.ps1:28-33`（递归找 `Live2DCubismCore.(dll|so|dylib)`，命中即 fail）与 `.github/workflows/release.yml:467-474`（`build-release` 下除 `vendor/` 外不得出现 Core）；同时 `:58` 强制包内必须有 `libbongo_live2d.so/.dylib`。
5. **X 缺 Core 时不是"崩"，是"降级 + 提示"**：插件描述符带 `BONGO_CAT_PLUGIN_REQUIRES_CORE`，加载期发现没有 Core 就拒绝加载并给 `BONGO_CAT_ERROR_CUBISM`（`model_plugin_loader.c:38-44`），宿主继续用诊断 stub（`src/live2d/live2d_stub.c:1-9`），打开设置窗口时弹一次 toast（`preferences_lifecycle.c:152-163`）。
6. **X 的插件 ABI 六件套齐备、可直接抄设计**（不要抄二进制，AGPL 同源要署名）：版本化描述符 + `struct_size` + 必需回调子集 + 可选扩展独立符号 + 能力位协商 + 宿主服务表禁跨 CRT（详见 §6，全部有 `文件:line`）。
7. **我们现状与目标差距有 5 处硬事实**：Core 目前是**静态链接进 exe**（`cmake/Cubism.cmake:11,29-48,90`）；Windows 便携版是**单文件 exe**（`cmake/Packaging.cmake:43-55`、`release.yml:144`）；Windows 关掉了 SDL 对话框（`cmake/Dependencies.cmake:45-47`）；着色器以**散文件**进包（`cmake/CubismAssets.cmake:64-68`、`check-unix-package.ps1:37-40`）；发布说明里**公开了 Core 的 SHA-256**（`release.yml:160-176`、`ci.yml:386-400`）——最后这条等于自证分发。
8. **我们不需要重写 347 个调用点**：照 X 的 `src/live2d/live2d_dispatch.c` 模式，把现有 `bongo_cat_live2d_*`（67 个符号）在宿主内改成"判空转发到 ops 表"，`src/live2d/live2d_stub.c` 变成**永远编译的兜底 ops**，插件只是另一套 ops。
9. **最大技术风险是"双后端"**：我们的 Vulkan 桥把宿主私有的 `struct BongoCatVk *` 和裸 `VkImage/View/Sampler` 直接穿过 API（`include/bongo_cat/live2d_vulkan.h:37-79`，合成侧 `src/render/vk/vk_scene.c:36-150`），这个结构**不能跨 DLL 边界**。建议分阶段（先 GL 插件，VK 走宿主服务表 + 插件自持离屏目标）。
10. **建议路线**：随包发已编译插件（AGPL 源码同源、署名）+ 用户本地导入 Core；**v1 不做应用内代下载**（X 做了，法律上更激进）；离线用户可整体拷贝 `live2d-core/` 目录。

---

## 2. X 的许可 / 同意流程

### 2.1 仓库与发行物的许可声明

| 位置 | 原文 / 要点 |
| --- | --- |
| `README.md:19` | AGPL-3.0 徽章 `<a href="LICENSE"><img src="…AGPL--3.0…">` |
| `README.md:16` | `> 本仓库（**BongoCat-X**）是 [vladelaina/BongoCat] 的 fork 分支。本仓库与 Live2D 公司无关联、不附带 Live2D Cubism SDK，详见下方「Live2D 声明」章节。` |
| `README.md:61-63` | **`## ⚠️ Live2D 声明`**：`本仓库与 Live2D 公司（Live2D Inc.）及其官方项目无任何关联。Live2D Cubism SDK 为 Live2D Inc. 的专有软件：本仓库不附带、不内置、也不分发该 SDK。构建前需按下方「Live2D / Cubism SDK」章节的说明，从 Live2D 官网自行下载并导入，使用须遵守 Live2D 的许可协议。` |
| `docs/README.en-US.md:75-77` | 同样内容的英文版 `## ⚠️ Live2D Disclaimer`（另 8 个语言 README 同构，共 10 个） |
| `resources/assets/models/LICENSE:10` | 内置模型资产 MIT，但 `it does not change the AGPL-3.0-only license of the BongoCat source`（证明主许可 = AGPL-3.0-only） |
| `AGENTS.md:36-47` | 给协作者的硬约束：`The plugin never links or embeds Cubism Core; the host loads user-supplied Core externally. Static Core linking is unsupported, and packages must contain no SDK.` / `Plugins are managed on the Preferences Plugins page, in <data>/plugins; bundled plugins live beside the executable or in macOS Contents/PlugIns.` |
| `.github/workflows/release.yml:1-8` | `# Runtime-Core release build: CI downloads the Cubism SDK straight from the official site to compile the Cubism Framework and bridge, but the Core binary itself is never shipped … The workflow is guarded to the fork and maintained by a user who accepted the Live2D Proprietary Software License Agreement (see README).` |

**没有**独立的 `NOTICE` / `THIRD-PARTY` 文件（`resources/assets/licenses/` 只有 `libwebp.txt`、`nanosvg.txt`）——Live2D 相关的声明全部集中在 README/Disclaimers 与应用内同意窗。

### 2.2 "Core 由用户自行取得"的文案（发行说明侧）

`README.md:51-57`（原文）：

```
> [!IMPORTANT]
> 官方 Release 为 **runtime-Core 构建**：内置 Live2D 渲染支持，但**不附带** Live2D Cubism Core 运行库
> （专有授权，不随包分发），**并非**无 Live2D 能力的诊断构建。未检测到 Core 时会回退到诊断后端并在设置窗口提示。

**启用 Live2D 渲染（任选其一）：**
1. **应用内导入（推荐）**：打开「设置 → 插件」页，点击「导入 Live2D Core」，选择 `Live2DCubismCore.dll`
   或官方 Cubism SDK 的 zip 压缩包，导入后立即生效（无需重启）。
2. **live2d 文件夹投放**：从 [Cubism SDK 下载页面](https://www.live2d.com/en/sdk/download/native/)
   （需同意 Live2D 许可协议）下载 **Cubism SDK for Native**，将 zip 或解压出的 `Live2DCubismCore.dll`
   放入应用目录/数据目录下的 `live2d` 文件夹，重启应用后自动识别启用。
```

`docs/README.en-US.md:491` 一行总结运行期行为：`The application and packages contain no SDK or Core.`

### 2.3 应用内同意流程（逐步 + 文件:line）

| 步骤 | 证据 |
| --- | --- |
| 触发点 1：插件页点"启用"且无 Core | `src/ui/preferences/preferences_plugins.c:75-76` |
| 触发点 2：插件页点 `Download Live2D SDK`（i18n 键 `pages.preference.live2dSetup.getSdk`） | `preferences_plugins.c:91-92` |
| 触发点 3：导入的插件 ZIP 里含 Live2D 插件且当前无 Core | `preferences_plugins.c:42-44` |
| 同意窗 | `src/ui/preferences/preferences_live2d_setup_dialog.c:30-109`；标题 `:43-44`、说明 `:46`（Windows）/`:48`（其它平台）、协议正文滚动区 `:78-94`、按钮 `:95-106` |
| Windows 说明原文 | `Cubism Core is missing. Read both official agreements before downloading. Cancel keeps the plugin enabled without rendering.`（`:46` 兜底文案；真实文案在 locales，见下） |
| 非 Windows 原文 | `Automatic Core installation is available on Windows. Import a compatible Core manually on this platform.`（`:48`），且同意按钮被强制禁用 `:102-104` |
| 按钮 | `Cancel and continue`（`:98-99`）/ `Agree and download`（`:105-106`，`ready` 要求 `licenses_ready` 且相位为 ready/failed） |
| 抓两份官方协议正文 | `preferences_live2d_setup.c:24-47`；URL 模板 `https://www.live2d.com/eula/live2d-%s-software-license-agreement_%s.html`，`%s` = `proprietary` / `open`（`:29-32`），语言按当前 UI 语言取 `cn`/`en`（`:90-91`）；上限 1 MiB（`:33`）；HTML→文本在 Rust 侧 `bongo_safe_live2d_license_text` |
| 点同意后才下载 | `preferences_live2d_setup.c:108-119`（`accept`：相位从 `LICENSE_READY` → `SDK_DOWNLOADING`，起后台线程） |
| 下载与提取 | `src/rust/bongo-safe/src/live2d_download.rs:100-149`（`bongo_safe_live2d_download`）；目标路径 `<data_root>/live2d/downloads/Live2DCubismCore.dll`（`preferences_live2d_setup.c:92-94`） |
| 完成后导入 + 清临时文件 + 提示 | `preferences_live2d_setup.c:130-144`；成功文案键 `pages.preference.live2dSetup.installed`（`:137`），并 `bongo_cat_file_remove(job->target)`（`:138`） |
| 取消的语义 | 保留插件启用、不渲染（文案见 locales `description`）；`stop()` 会删掉半途的临时 Core 文件（`:74-81`） |

### 2.4 下载源与提取白名单（关键法律事实）

* 固定 URL：`https://cubism.live2d.com/sdk-native/bin/CubismSdkForNative-5-r.5.zip`（`live2d_download.rs:12`）。
* 提取路径**只有一个**（`live2d_download.rs:53-56`）：
  `CubismSdkForNative-5-r.5/Core/dll/windows/{x86_64|x86}/Live2DCubismCore.dll`
* 限制：条目数 ≤ 20000（`:50`）、单文件 ≤ 64 MiB（`:13,60,78`）、首 2 字节必须是 `MZ`（`:81-83`）、先写临时文件再 `persist`（`:66-96`）；**不做哈希/签名校验**（见 §7）。
* 对比：CI 侧的 SDK 还原脚本**有**固定 SHA-256：`.github/scripts/restore-cubism-sdk.ps1:8-12`（`7FF3A4BBC19C0A8728965AA522AB77EB11B252916453E68A8A78D3B71188BB12`）+ GLEW 2.2.0 哈希（`:18-22`）。

### 2.5 手动导入（文件选择）与"投放文件夹"

* 文件选择：`src/ui/preferences/preferences.c:107-124`，`SDL_ShowOpenFileDialog` + 过滤器 `{"Live2DCubismCore.dll / SDK zip", "dll;zip"}`（Windows）/`dylib;zip`（macOS）/`so;zip`（其它）（`:109-117`）。
* 平台导入实现：Windows `src/platform/windows/windows_live2d_sdk.c:377-449`；POSIX `src/platform/posix/posix_live2d_sdk.c:327-397`（同一套语义）。
* 启动期自动发现顺序（Windows）：`HKCU\Software\BongoCat\Live2D\CoreDll` 注册表缓存 → exe 旁裸 `Live2DCubismCore.dll` → `<base>/live2d` 与 `<data>/live2d` 里扫裸 DLL 与任意 `*.zip`（`windows_live2d_sdk.c:327-366`，注释在 `:1-4`）。
* "发现即入库"：确认可加载后复制一份到 `<data>/live2d/Live2DCubismCore.dll` 并把注册表指向副本（`windows_live2d_sdk.c:253-268`）。
* 找不到时不报错退出，而是**把 `live2d/` 目录留出来当投放点**（`windows_live2d_sdk.c:362-365` 注释原文：`Nothing loaded: leave the live2d folders behind as a visible drop-in place for the Core library or the SDK zip.`）。
* 插件 ZIP 里夹带的 Core 也走同一个原生导入器，且 Core **不落在插件管理目录**：`src/runtime/model/plugins/model_plugin_archive.c:60-63`（`Core remains external to the renderer's managed plugin directory.`），测试断言 `data/plugins/Live2DCubismCore.dll` 不存在（`tests/model_import/test_plugin_archives.py:45`）。

### 2.6 十语言文案（现成的键名可直接借鉴）

`resources/assets/locales/*.json:10` 起是 `pages.preference.live2dSetup` 段，10 个语言齐备（`de-DE, en-US, es-ES, fr-FR, ja-JP, ko-KR, pt-BR, ru-RU, zh-CN, zh-Hant`），键为：
`title / description / loading / downloading / failed / installed / cancel / accept / getSdk / unsupported`。

原文（`en-US.json:10-20`、`zh-CN.json:10-20`）：

```jsonc
"description": "Cubism Core is missing. Please read both official Live2D agreements below. Agreeing starts an in-app SDK download and installs the matching Core. Cancelling keeps the plugin enabled without Live2D rendering.",
"accept": "Agree and download",
"cancel": "Cancel and continue",
"getSdk": "Download Live2D SDK",
"unsupported": "Automatic Core installation is available on Windows. You can import a compatible Core manually."
// zh-CN: "缺少 Cubism Core 运行库，请阅读下方两份 Live2D 官方协议。同意后将在应用内下载 SDK 并安装适配的 Core；取消后继续启用插件，但不进行 Live2D 渲染。"
//        "同意并继续下载" / "取消并继续" / "下载 Live2D SDK"
```

另有一组"构建类型 / 缺 Core"的提示键，10 语言齐备（`en-US.json:469-470`）：

* `live2dSdkMissing`：`This build does not include Live2D support (diagnostic build), so Live2D rendering is unavailable. Please use a build shipped with Live2D support.`
* `live2dCoreMissing`：`Live2D Cubism Core not found: import the Core library or the official SDK zip in the settings window, or drop the zip into the live2d folder (next to the application or inside the data directory) and restart.`

---

## 3. X 的构建期守卫

### 3.1 硬守卫：拒绝静态链接 Core

`CMakeLists.txt:11-16`（原文）：

```cmake
option(BONGO_CAT_RUNTIME_CORE
  "Load user-supplied Cubism Core externally; static Core linking is unsupported" ON)
if(NOT BONGO_CAT_RUNTIME_CORE)
  message(FATAL_ERROR "BONGO_CAT_RUNTIME_CORE=OFF is unsupported: renderer plugins "
    "must not embed Cubism Core. Supply Core separately at runtime.")
endif()
```

`cmake/Cubism.cmake:34`（原文，一句话纪律）：

```cmake
# Core is never linked into the plugin. Only its API header is needed here.
```

### 3.2 机制：生成的 shim 持有 `csm*` ABI，运行时转发

* `cmake/Cubism.cmake:47-48`：`# The generated shim owns the csm* ABI and forwards calls through host callbacks to the user-supplied shared Core library on every platform.`
* 生成规则：`cmake/Cubism.cmake:49-59` 调用 `cmake/gen_core_shim.py`，输入是 **SDK 自带的 `Core/include/Live2DCubismCore.h`**（只读头文件，不读 `.lib`/`.a`），输出 `generated/cubism_core_shim.c`，再 `add_library(bongo_cat_core_shim STATIC …)`；链接 `${CMAKE_DL_LIBS}`（`:63-64`）。
* shim 的解析函数（`cmake/gen_core_shim.py:30-35`）：

```c
static void *core_resolve(const char *name, void **slot) {
    if (!*slot) *slot = bongo_cat_model_plugin_host()->core_symbol(name);
    return *slot;
}
```

* 每个 `csmApi` 声明生成一个转发器，找不到符号时返回 0/不动（`gen_core_shim.py:94-109`）；生成期还做健全性检查：声明数 < 40 或重名就退出（`:68-74`）——防止 SDK 头格式变了却静默生成空 shim。
* Windows 额外定义 `CSM_CORE_WIN32_DLL=1`（`Cubism.cmake:44`），因为 Core 是 DLL 而不是静态库；这个宏的由来有踩坑记录（`CHANGELOG.md:237`：x86 下按 cdecl 声明而导入库是 stdcall 装饰名，出现 52 个未解析外部符号）。
* 宿主侧的服务实现只有一行：`src/runtime/model/plugins/model_plugin_services.c:15-17` `.core_symbol = bongo_cat_platform_live2d_core_symbol`。

### 3.3 target 依赖纪律：宿主不链 Framework，插件才链

* `cmake/ModelPlugins.cmake:2`：`# Renderer implementations are shared libraries; the host never links Framework.`
* 宿主的源集合只含 stub + dispatch + loader（`ModelPlugins.cmake:4-11`），插件 `bongo_live2d` 才链 Framework（`:46-50`：`add_library(bongo_live2d SHARED …)` + `target_link_libraries(bongo_live2d PRIVATE Framework bongo_cat_warnings)`）。
* 插件编译期定义 `BONGO_CAT_LIVE2D_CORE_RUNTIME=1`（`ModelPlugins.cmake:58`），并按后端附加 `BONGO_CAT_HAS_CUBISM_VULKAN/METAL`（`:59-63`）。
* 可见性收紧：`CXX_VISIBILITY_PRESET hidden` 等（`ModelPlugins.cmake:64-65`），只有描述符与两个扩展符号导出（`src/live2d/cubism_plugin.cpp:32-36` 的 `PLUGIN_EXPORT`）。
* 宿主 target 实际链接列表：`CMakeLists.txt:229`（`add_executable(bongo_cat src/main.c)`）、`:240`（只链 `bongo_cat_runtime bongo_cat_warnings`）。

### 3.4 "踩过坑"的注释原文（值得抄进我们的注释）

| 文件:line | 原文 | 教训 |
| --- | --- | --- |
| `cmake/Cubism.cmake:99-103` | `# The upstream Framework is not warning-clean; keep project /Werror flags from reaching it (release CI builds with warnings-as-errors).` | 第三方树要隔离 `/WX`、`-Werror` |
| `cmake/ModelPlugins.cmake:37-39` | `# Complete shader compilation before the embedding rule examines binary dependencies. MSBuild otherwise emits the same output rules in both projects and may execute them twice in one parallel build.` | MSBuild 并行下重复执行自定义命令 |
| `cmake/ModelPlugins.cmake:126` | `# Restage changed DLLs even when the executable needs no relink.` | 只靠 POST_BUILD 会在 exe 不需要重链时漏掉插件更新 |
| `src/platform/windows/windows_live2d_sdk.c:362-365` | `# (Nothing loaded: leave the live2d folders behind as a visible drop-in place …) SDL3 owns the SDL_GetBasePath() string; freeing it here made every later rescan hit a double free inside SDL_free.` | `SDL_GetBasePath()` 是 SDL 自有内存，重复 free 会堆损坏（`CHANGELOG.md:248` 记录了 0xC0000374 闪退） |
| `CHANGELOG.md:238` | `csmGetVersion()` 返回数值版本号（`unsigned int`），运行时加载器误按字符串指针解引用传给 `%s`，崩溃在 printf 内部 | Core 版本要按整数打印 |
| `CMakeLists.txt:198-204` | VS 生成器会把 `$<COMPILE_LANGUAGE:C>` 从 PCH 编译里丢掉，导致 C 源文件报 C2855 | PCH 与 MSVC 生成器要互斥 |

### 3.5 插件 staging（打包侧）

`cmake/ModelPlugins.cmake:118-155` 的 `bongo_cat_stage_model_plugins(target)`：

* 目标目录：非 Apple = `<exe 目录>/plugins`（`:121-123`），Apple = `Contents/PlugIns`（`:120`）；
* `copy_if_different` + 独立 `add_custom_target(..._stage_<plugin>)` 依赖（`:127-132`）；
* Apple 对每个嵌套二进制 `codesign --force --sign -`（`:142-147`），并登记 `LINK_DEPENDS` 以便重新封签（`:133-136`）；
* `install(FILES … DESTINATION plugins COMPONENT Runtime)`（`:151`）。

发行形态后果（`docs/model-plugins.md:35-36`）：`Windows portable releases are ZIP archives containing the executable and optional plugins directory. Extract the entire archive; keep the plugins with the executable.`

### 3.6 编译期开关与"诊断后端"措辞

* `option(BONGO_CAT_REQUIRE_CUBISM … OFF)`（`CMakeLists.txt:8-10`）；缺失且 `ON` → `FATAL_ERROR`，错误信息里包含官网链接、许可协议名、重命名目录说明、GLEW 补充步骤、VS2022 要求、"想只要诊断后端就 `-DBONGO_CAT_REQUIRE_CUBISM=OFF`"（`:104-120`）。
* 缺失且 `OFF` → `WARNING`（`:121-128`），原文关键句：`Live2D requires a compatible plugin built with the SDK (-DBONGO_CAT_REQUIRE_CUBISM=ON) to produce a binary that loads the Core at runtime.`
* 宿主永远定义 `BONGO_CAT_LIVE2D_CORE_RUNTIME=1`（`CMakeLists.txt:188-191`），因为 Core 一律运行期加载。

---

## 4. X 的 CI / 包体检查

### 4.1 发行包"不含 Core"的断言

| 脚本/工作流 | 关键行 | 内容 |
| --- | --- | --- |
| `.github/scripts/check-unix-package.ps1` | `:28-33` | 注释 `# No build ships the Core binary - users supply it at runtime.`；递归扫 `^Live2DCubismCore\.(dll\|so\|dylib)$`，命中抛 `Core binary leaked into package: …` |
| 同上 | `:58` | Live2D 包必须含 `plugins/libbongo_live2d.<so\|dylib>`；`:59-65` 还要求 Inox2D 插件与 `licenses/Inox2D-LICENSE`；`:72-73` 禁止散落 `LICENSE` 文件 |
| 同上 | `:78-94` | smoke 测试**模拟用户行为**把 SDK 里的 `<sdk>/Core/dll/linux/x86_64/libLive2DCubismCore.so` 复制到 `<storage>/data/live2d/`，走真实的运行期导入路径（注释原文：`Runtime-Core packages ship without the Core binary; drop it in the same way an end user would …`） |
| `.github/workflows/release.yml` | `:304-306` | `# Compiles the Cubism Framework + bridge into the runtime-Core binary. The Core binary extracted here is build-time only: the package steps assert that no Live2DCubismCore.* ever reaches the release output.` |
| 同上 | `:467-474` | Windows 侧：`Get-ChildItem build-release -Recurse -Filter 'Live2DCubismCore.*' \| Where-Object { $_.FullName -notmatch '\\vendor\\' }` → 命中抛 `Core binary outside vendor/` |
| 同上 | `:506-514` | Unix 侧调用 `check-unix-package.ps1` 并传 `ExpectLive2D/ExpectInox2D/ExpectStatistics = $true` |
| 同上 | `:524-528` | Linux 额外跑 `.github/scripts/check-appimage.sh`（AppImage 也要过同样的包体检查） |

### 4.2 "SDK 只能在被守卫的地方还原" 的策略脚本

`.github/scripts/check-publish-guards.ps1`：

* 头部政策注释（`:1-9`）原文关键句：`The Live2D Cubism SDK is proprietary. Restoring it in CI is allowed only in release.yml (runtime-Core release builds: the maintainer has accepted the Live2D Proprietary Software License Agreement) and in mac-app-store.yml. SDK-restoring jobs must additionally be guarded to the fork, and the Core binary must never be shipped in packages.`
* `cubismSdkMarkers`（`:28-36`）会匹配 `restore-cubism-sdk`、`-DBONGO_CAT_REQUIRE_CUBISM=ON`、`-RequireCubism`、`CUBISM_SDK_ARCHIVE_URL`、`vendor/CubismSdkForNative`、`cubism.live2d.com`、`CubismSdkForNative-`；
* 白名单只有 `release.yml` 与 `mac-app-store.yml`（`:37`），其它工作流只要出现任一 marker 就失败（`:47-55`）；
* 发布类 job（含 `actions/upload-artifact`、`softprops/action-gh-release`、`build-store-package.ps1`、`VIRUSTOTAL_API_KEY`）必须带 repo guard（`:21-27`、`:64-75`）；
* 还原 SDK 的 job 必须**再**带一层 fork guard（`:76-88`）；`-SelfTest` 用两个反例自检（`:127-147`）。
* 还原脚本本身也设防：`restore-cubism-sdk.ps1:50-57` 拒绝把 SDK 释放到 `vendor/CubismSdkForNative` 以外的目录（防止路径参数写错覆盖别的东西）。

### 4.3 构建树里的 Core（合法但要注意）

`cmake/Tests.cmake:471-491`：runtime-Core 模式下，为了让真正跑 Live2D 的单测能找到 Core，会把 `<sdk>/Core/dll/<平台>/…` 复制到测试可执行文件旁边（`:493-497` 的 POST_BUILD）。这是**构建树**行为，不进发行包。

---

## 5. X 缺 Core / 缺插件时的降级路径

### 5.1 三种缺失的处理

| 情形 | 判定点 | 结果 |
| --- | --- | --- |
| 插件未安装 | `model_plugins.c:16-24`（按平台拼 `bongo_live2d.dll` / `libbongo_live2d.so` / `.dylib`）+ `:34-50`（找不到文件） | `SDL_LoadObject(NULL)` → 报 `Model plugin is unavailable: <filename> (<SDL_GetError>)`（`model_plugin_loader.c:15-20`） |
| 插件被禁用（`.disabled` 标记） | `model_plugins.c:30-33` | `Renderer plugin is disabled`（`model_plugin_loader.c:12-14`），不加载 |
| 插件存在但缺 Core | 描述符 `flags & BONGO_CAT_PLUGIN_REQUIRES_CORE` 且 `!bongo_cat_platform_live2d_core_available()` | `BONGO_CAT_ERROR_CUBISM` + `Live2D plugin requires the user-supplied Cubism Core`（`model_plugin_loader.c:38-44`），**卸载插件库**，宿主继续用 stub |
| ABI / engine / 后端不匹配 | `model_plugin_loader.c:25-37` | `Model plugin ABI or engine does not match this application` / `<name> does not support the selected graphics backend` |

### 5.2 降级后程序还能做什么

* `src/live2d/live2d_stub.c` 是**完整的一套中性实现**，被编进宿主（`ModelPlugins.cmake:4-11` 的宿主源列表含 `live2d_stub.c`），保证"编译、链接、启动到主界面"（头注释 `:1-9` 原文：`它的目的不是"还能跑 Live2D", 而是让整个程序在没有 SDK 的情况下依然能编译、链接并启动到主界面…`）。
* 中性值纪律：查询类返回 false/空；但"返回 false 会让上层整块关掉功能"的查询要返回中性值（同样在 `:1-9`），例如 stub 给出合法的画布尺寸而不是 0（`:19-22`）。
* 函数名复用靠宏改名：`ModelPlugins.cmake:12-22` 把宿主要用的 `bongo_cat_model_runtime_<m>` 映射到 `stub_model_runtime_<m>`，于是 stub 与插件共用同一套调用点。
* 分发调用的统一形态在 `src/live2d/live2d_dispatch.c:3-155`：**每个函数都先判空再转发**，缺失的回调返回 false / `-1` / 不动（例如 `:148-151` 的 `expression` 返回 `-1`）。

### 5.3 UI 如何提示

* 打开设置窗口时**每次运行只弹一次**的 toast：`src/ui/preferences/preferences_lifecycle.c:152-163`。判定条件是"Live2D 引擎已选中但不可渲染"，并按"插件是否已安装且启用"选择文案键：已装 → `native.live2dCoreMissing`；未装/被禁 → `pages.preference.plugins.live2dMissing`（`:160`），兜底文案 `Install and enable the Live2D renderer on the Plugins page, then supply Cubism Core.`（`:161`），Toast 时长常量 + 悬停暂停（`:158-162`）。
* 插件页常驻入口：`preferences_plugins.c:85-93`
  * `Import Cubism Core` 按钮 → 文件选择（`:87-88`）；
  * 插件已启用时再给 `Download Live2D SDK` 按钮 → 同意流程（`:89-93`）；
  * 插件行状态文案：`In use / Not installed / Disabled / Installed`（`:58-62`）；
  * 插件卡片描述（`:64-66`）：`Live2D models require Cubism Core. Missing Core can be imported with the ZIP.`
  * 拖入区提示（`:106-107`）：`Click or drop a plugin ZIP here` / `ZIP or renderer library; missing Core is imported from the package.`
* 设置窗口标题栏/资源区留出的高度按"有没有 Core"分档（`:137`），避免按钮被裁掉。
* 插件 ZIP 导入成功后若缺 Core，立即弹同意流程（`:42-44`）。

---

## 6. X 的插件 ABI 六件套

> 用途：作为**我们自己插件 ABI 的参考设计**。不复制 X 的任何二进制；X 是 AGPL-3.0-only，若我们直接搬运其头文件/源码片段，需要保留 AGPL 与署名（本方案默认"自行实现、设计同源、注释里署名"）。

### ① 版本化描述符 + 单一导出符号

* `include/bongo_cat/model_plugin.h:9-10`：
  `#define BONGO_CAT_MODEL_PLUGIN_ABI 1u` / `#define BONGO_CAT_MODEL_PLUGIN_SYMBOL "bongo_cat_model_plugin_query"`
* 查询签名（`:93-94`）：`typedef const BongoCatModelPlugin *(*BongoCatModelPluginQuery)(uint32_t abi, const struct BongoCatModelPluginHost *host);`
* 双向校验（插件侧也验宿主）：`src/live2d/cubism_plugin.cpp:55-58`
  `if (abi != BONGO_CAT_MODEL_PLUGIN_ABI || !host || host->abi_version != abi || host->struct_size != sizeof(*host)) return nullptr;`

### ② `struct_size` 结构体尺寸校验（拒绝 ABI 漂移）

* 描述符字段顺序（`model_plugin.h:88-92`）：`uint32_t abi_version, struct_size, engine, flags, graphics_backends; const char *name; BongoCatModelPluginOps ops;`
* 宿主校验（`src/runtime/model/plugins/model_plugins.c:82-83`）：`p->abi_version == BONGO_CAT_MODEL_PLUGIN_ABI && p->struct_size == sizeof(*p)`；
* 设计意图注释（`model_plugin.h:36-38`）：`Same compiler ABI within an architecture; size/version reject drift. Instances, jobs and GPU resources must be destroyed before unloading.`

### ③ 必需回调子集（其余全部可选）

* 必需 8 个（`model_plugins.c:84-85`）：`create / destroy / load_ex / ready / update / draw_checked / resize / set_rhi_info`。
* 其余约 39 个回调只要**判空调用**即可（`live2d_dispatch.c` 全文即此模式），没有"必须实现"的负担。

### ④ 可选扩展 = 独立导出符号（老宿主不认识就跳过）

* `BONGO_CAT_PLUGIN_OPTIMIZE_SYMBOL "bongo_cat_model_plugin_optimize_v1"`（`model_plugin.h:14`，注释 `:11-13`：`Optional, additive extension: legacy plugin descriptor/host ABI stays v1 … Missing = none.`）
* `BONGO_CAT_PLUGIN_VULKAN_FEATURES_SYMBOL "bongo_cat_model_plugin_vulkan_features_v1"`（`model_plugin.h:25`，注释 `:22-24`：`Old hosts omit it, so new plugins must default to no enabled optional device features.`）
* 加载方式（`model_plugin_loader.c:53-61` 与 `:66-70`）：各自 `SDL_LoadFunction`，**找不到就 `SDL_ClearError()`**，不视为错误、不影响加载。

### ⑤ 能力位协商（三方交集）

* `flags`：`BONGO_CAT_PLUGIN_REQUIRES_CORE`、`graphics_backends`：`OPENGL / VULKAN / METAL`（`model_plugin.h:32-35`）。
* 宿主先校验"插件支持当前后端"（`model_plugin_loader.c:31-37`），再校验"需要 Core 时 Core 在不在"（`:38-44`），最后才 `ops.create`（`:45-46`）。
* 优化开关是**请求 ∩ 支持 ∩ 插件实际返回**（`:62-69`）：`target->optimizations = fn ? fn(instance, requested, backend) & requested : 0;`

### ⑥ 宿主服务表：禁跨 CRT / 禁跨运行时所有权

* 表头就是版本 + 尺寸 + 第一个服务是 Core 符号解析（`include/bongo_cat/model_plugin_host.h:19-21`）。
* 核心约定（`model_plugin_host.h:17-18` 原文）：`Services stay owned by the host; plugins never link another SDL runtime. Every allocation is released through the service which created it.`
* 具体落地例子：插件侧**自己实现** `bongo_cat_file_open` 以保证 `FILE*` 留在本 DLL 的 CRT 内（`src/live2d/cubism_plugin.cpp:19-31`，注释 `FILE handles stay within this DLL's CRT.`）；宿主把 `SDL_*`、图像、JSON、SHA-256 等全部以函数指针形式提供（`model_plugin_host.h:22-99`，约 80 个服务）。
* 插件不得链接自己的 SDL：`ModelPlugins.cmake:51-53` 只给头文件（`target_include_directories(bongo_live2d SYSTEM PRIVATE $<TARGET_PROPERTY:SDL3::SDL3-static,INTERFACE_INCLUDE_DIRECTORIES>)`），注释 `Headers are needed for the callback table, but SDL is owned by the host.`

### 补充：查找 / 安装 / 启停（这一套同样值得抄）

* 解析顺序（`model_plugins.c:34-50`）：`BONGO_CAT_PLUGIN_DIRECTORY` 环境变量 → `<data_root>/plugins/` → `<exe 目录>/plugins/` → macOS `<exe>/../PlugIns`；每一步都要求是文件。
* 管理目录 = `<data_root>/plugins`（`model_plugins.c:11-15`）。
* 停用标记 = 同名 `.disabled` 空文件（`:30-33`、`:71-80`）。
* 安装流程（`:87-109`）：大小上限 256 MiB → 若是 ZIP 走归档安装 → 先 `SDL_LoadObject` + 查描述符确认"这是我们的插件、是哪个引擎" → 再写 `.new` 后 `bongo_cat_file_replace` 原子替换 → 置为启用。
* 活动插件不允许替换（`:62-65`：`Switch to another renderer before changing an active plugin`）。
* 文档口径（`docs/model-plugins.md:11-17`）把顺序与 env 覆盖写清楚，便于排障。

---

## 7. X 的弱点 —— 哪些不要照抄

1. **不校验下载物完整性**：`live2d_download.rs` 只验 `MZ` 头 + 大小上限，没有哈希/签名（对比 CI 的 `restore-cubism-sdk.ps1:8-12` 反而有固定 SHA-256）。我们应记录并在 UI 显示 SHA-256，且不把"官方 zip 的哈希白名单"写死（官方会更新版本）。
2. **SDK zip 全量解压**：`extract_sdk_zip` 会把压缩包里**每个**条目写盘（只做基本 zip-slip 串检查：`windows_live2d_sdk.c:211-213` 用 `strstr(name, "..")`，会误杀合法文件名如 `a..b`，也漏掉 `C:\`、UNC、符号链接），几十 MB/上万个文件。我们应改成**条目白名单**：只落地匹配本机平台/架构的那一个 Core 文件。
3. **没有符号链接/设备文件判定**：X 完全依赖 miniz 的"普通文件/目录"两态，没有检查 zip 外部属性里的 symlink 位。
4. **崩溃隔离为零**：插件与 Core 都在宿主进程里 `dlopen`，插件或 Core 崩 = 进程崩（X 没有任何隔离层或沙箱承诺）。我们要在发布说明与风险里对用户讲清楚。
5. **注释与实际实现漂移**：`cmake/Tests.cmake:471-477` 说 Windows 用 delay-load 加载 Core 导入库，但整树 grep（`delayload` / `DELAYLOAD` / `Live2DCubismCore.lib` / `IMPORTED_IMPLIB`）**零命中**——实际机制是 shim。抄注释前先验证。
6. **插件自实现 `bongo_cat_file_open`** 这类"靠约定维持 CRT 边界"的地方没有自动化 ABI 测试兜底（X 侧未见专门的反例测试）；我们应补"坏描述符矩阵"单测。
7. **`AGENTS.md:44-46` 的纪律靠文档**，没有 CMake 层面的"插件不得依赖 Core 导入库"的可执行断言（只有 `BONGO_CAT_RUNTIME_CORE` 这个总开关 + 注释）。我们可以在产物层面加断言（见 §9.6）。

---

## 8. 我们的现状（为什么非改不可）

| 事实 | 证据 | 影响 |
| --- | --- | --- |
| Core 是**静态导入库**直接链进 exe | `cmake/Cubism.cmake:11`（`add_library(Live2DCubismCore STATIC IMPORTED GLOBAL)`）、`:29-30`（`…/143/Live2DCubismCore_MT.lib`）、`:37-41`（macOS/Linux `.a`）、`:90`（`target_link_libraries(Framework PUBLIC Live2DCubismCore glew_s)`）；Framework 又 PUBLIC 链进宿主 `CMakeLists.txt:126-129` | **发行包含专有库的目标码**，法律风险最高的形态 |
| 发布说明公开 Core 的 SHA-256 | `.github/workflows/release.yml:159-161,174`；`.github/workflows/ci.yml:386-400` | 等于自证"我们把它打进了包" |
| Windows 便携版 = 单文件 exe | `cmake/Packaging.cmake:43-55`（`copy_if_different "$<TARGET_FILE:bongo_cat>" "…-portable.exe"`）、`release.yml:144` | 与"必须带 `plugins/` 子目录"直接冲突 |
| Windows 关闭 SDL 对话框 | `cmake/Dependencies.cmake:45-47`（`if(WIN32) set(SDL_DIALOG OFF …)`）；SDL 侧确认 `SDL_ShowOpenFileDialog` 在 `SDL_DIALOG_DISABLED` 下只是报错的桩（`_deps_cache/sdl3-src/src/dialog/SDL_dialog.c:31,67-92`） | 当前**无法**弹出文件选择器；仓库内 `SDL_ShowOpenFileDialog` 零命中 |
| 目前只有拖放导入（`SDL_EVENT_DROP_FILE`），没有文件选择器 | `src/ui/preferences/preferences.c:177,334` | 引导流程要新增对话框能力 |
| 着色器以散文件进包并被断言 | `cmake/CubismAssets.cmake:38-68`（POST_BUILD `copy_directory` + `install(DIRECTORY … assets/FrameworkShaders)`）；`.github/scripts/check-unix-package.ps1:37-40` 要求 4 个 `FrameworkShaders/*` 文件；`src/runtime/shell/window.c:27-41` 找不到就拒绝 Vulkan 并回退 GL | 插件化后着色器应**内嵌进插件**（X 的做法：`ModelPlugins.cmake:23-36` + `cmake/embed_plugin_shaders.py`），包体检查随之改写 |
| 宿主与 Live2D 是同一次编译：`BONGO_CAT_CUBISM_ENABLED` 决定编 stub 还是真实现 | `CMakeLists.txt:44-45,47-82` | 插件化后宿主**永远**编 stub，真实现只在插件里；这是把"编译期二选一"改成"运行期二选一" |
| Live2D 的对外 API 面很大 | `include/bongo_cat/model.h`（`bongo_cat_live2d_*`）、`include/bongo_cat/live2d_vulkan.h:37-79`；全仓 67 个不同符号、约 347 处调用（含 stub 与头文件） | 不建议逐个改签名；照 X 加一层 dispatch |
| Vulkan 桥把宿主私有类型穿过 API | `include/bongo_cat/live2d_vulkan.h:14,37,54,61`（`struct BongoCatVk *`）与 `:78-79`（裸 `VkImage/View/Sampler`）；合成侧 `src/render/vk/vk_scene.c:36-69,112-150` | 这是插件边界最硬的一处，必须专门设计 |
| 现有 CI 包体检查**没有** Core 断言 | `.github/scripts/check-unix-package.ps1:32-52`（只查资源存在与散落 LICENSE）、`:53-67`（smoke 断言 `renderer=cubism-native`） | 改造后 smoke 要么把 Core 放进 `<storage>/live2d-core/`（照 X `check-unix-package.ps1:78-94`），要么改成"缺 Core 也能起来"的断言 |
| 现有发布守卫没有 Cubism 策略 | `.github/scripts/check-publish-guards.ps1:6-8`（`upstreamGuards` 只有上游仓库）、`:9-23`（markers 里没有 Cubism 相关项） | 要照 X 的策略脚本补 |

---

## 9. 我们的落地设计

### 9.1 总体架构：谁提供什么

```
[我们的发行包]                          [用户机器]
  BongoCatTurbo.exe   ← 宿主：只含 stub + dispatch + 插件加载器
  plugins/            ← 我们编译并签名的渲染插件（AGPL 同源、署名）
    bongo_live2d.dll/.so/.dylib   ← Framework 静态链进来；**不含 Core**
  assets/…            ← 模型/字体/图标（FrameworkShaders 不再需要散文件）
                                         │  用户在引导页/文件选择里导入
                                         ▼
                              <存储根>/live2d-core/<plat>/<arch>/
                                   Live2DCubismCore.dll/.so/.dylib   ← 用户自己的闭源二进制
                                   core.json                        ← 我们记录的元数据
```

三条不可越界的规则（照 X 的边界，用我们的话写进 `AGENTS.md`）：

1. 宿主与插件的链接列表里**不得出现** `Live2DCubismCore*`（静态库或导入库）；
2. Core 的 `csm*` ABI 由配置期生成的 shim 持有，运行期经宿主服务表 `core_symbol` 解析；
3. 发行包与 CI artifact 里**不得出现**任何 `Live2DCubismCore.*` 文件。

### 9.2 目录与查找顺序（用户明确要求的两条已并入）

**插件目录**

| 优先级 | 路径 | 说明 |
| --- | --- | --- |
| 1 | `$BONGO_CAT_PLUGIN_DIRECTORY/` | 环境变量覆盖（排障 / CI / 绿色版放别处）；与 X 同名，便于互相排障 |
| 2 | `<存储根>/plugins/` | **主路径**：Windows = `%LOCALAPPDATA%\BongoCat\plugins\`；便携模式 = `<exe>\BongoCatData\plugins\`；`--storage-root=X` = `X\plugins\` |
| 3 | `<exe 目录>/plugins/` | **回退**（我们发行包自带插件的位置） |
| 4 | `<存储根>/data/plugins/` | 兼容读取（若历史上曾写入；仅读不写，避免两处状态分叉） |
| — | macOS 额外 | `BongoCatTurbo.app/Contents/PlugIns/`（与 X 一致，便于将来上架形态） |

理由：我们的存储层是"六棵树"模型（`src/runtime/lifecycle/storage_paths.c:1-13,51-58`），`<存储根>/plugins` 与 `config/data/models/cache/state/logs` 平级，便携模式下天然落在 `BongoCatData/`，用户可见、可整体拷走；`<exe>/plugins` 作为回退保证"解压即用"。

**Core 目录（我的决定）**

采用 **`<存储根>/live2d-core/`**，理由：

1. **资产归属不同**：插件是我们发的、可被程序自动替换/升级；Core 是用户自己的闭源资产，误删代价高。分目录才能让"CI 断言发行包不含 Core"和"用户一键清空 Core"都成立（X 也刻意把 Core 挡在插件管理目录之外：`model_plugin_archive.c:60-61`、`test_plugin_archives.py:45`）。
2. **升级/回滚解耦**：插件版本与 Core 版本节奏不同；分目录后"换插件不动 Core、换 Core 不动插件"。
3. **便携模式整体搬走仍可用**（`BongoCatData/live2d-core/…` 跟着走）。
4. **不采用"由插件在同一根下自寻"**：Core 的 `dlopen/LoadLibrary` 必须由**宿主**做，原因是（a）加载要发生在插件选择之前（`REQUIRES_CORE` 的能力位协商需要先知道 Core 在不在）；（b）宿主必须能校验版本、显示 SHA-256、做导入/回滚与 UI 提示；（c）将来 Inox2D 之类不需要 Core 的插件不该被迫理解 Core 的布局。X 也是宿主持有 loader、插件只拿 `core_symbol`（`model_plugin_host.h:21`）。

推荐布局与查找顺序：

```
<存储根>/live2d-core/
  core.json                       # { schema, platform, arch, sdk_hint, sha256, size, csm_version, source, imported_at, previous }
  windows/x86_64/Live2DCubismCore.dll
  linux/x86_64/libLive2DCubismCore.so
  macos/arm64/libLive2DCubismCore.dylib
  .staging/                       # 导入过程中的临时区（失败即整目录删除）
```

解析顺序：`$BONGO_CAT_LIVE2D_CORE`（可选覆盖，指向文件或目录）→ `<存储根>/live2d-core/<plat>/<arch>/` → `<存储根>/live2d-core/`（裸文件）→ `<exe>/live2d-core/`（回退/投放点）→ 兼容 `<存储根>/data/live2d/`。每一级都必须通过"三符号探测"才算命中（照 X：`csmGetVersion` + `csmReviveMocInPlace` + `csmInitializeModelInPlace`，`windows_live2d_sdk.c:51-56`）。

### 9.3 用户流程（7 步）

**① 引导页（10 语言）** — 新增 `pages.preference.live2dSetup.*`（键名沿用 X 的命名，便于对照）：

要点（每语言 8-12 句，逐步展开，不塞成一段）：

1. 为什么需要这一步：Live2D Cubism Core 是 Live2D Inc. 的**专有**运行库，我们**不分发**它；
2. 去哪：`https://www.live2d.com/en/sdk/download/native/`（给出可点击链接 + 纯文本 URL）；
3. 需要**自己阅读并同意**两份官方协议（Proprietary Software License Agreement + 开源许可），同意是与 Live2D 之间的事，与我们无关；
4. 下哪个包：`Cubism SDK for Native`（我们按 `5-r.5` 构建与测试；更新的版本可能可用但不保证）；
5. 不需要装 SDK、不需要编译器、不需要解压——程序会从压缩包里只取出对应你系统的**动态 Core**；
6. 我们**不联网**下载、不上传任何东西（v1 决策，见 §14）；
7. 目标平台/架构提示（自动填：`Windows x86_64` 等）；
8. 离线做法：在另一台能联网的机器下载后，把 zip 或 Core 文件拷过来；
9. 按钮：`选择 SDK 压缩包或 Core 文件…` / `打开 Core 目录` / `我在官网下载好了`（打开浏览器）；
10. 失败/取消的语义：插件保持启用，但不渲染（照 X 文案口径）。

**② 文件选择** — `SDL_ShowOpenFileDialog`，过滤器 `Live2DCubismCore.dll / SDK zip`（`dll;zip`），**必须先打开我们的 `SDL_DIALOG`**（`cmake/Dependencies.cmake:45-47`）；非 Windows 默认已开。失败时要能提示"文件选择器不可用，请手动把文件放到 `<存储根>/live2d-core/` 并点重试"。

**③ 安全解压**（比 X 严格，逐条都是攻击面）：

| 规则 | 做法 |
| --- | --- |
| zip-slip | 按**路径分量**判定（拒绝 `..` 分量、绝对路径、盘符、UNC、`\\?\`），而不是 `strstr("..")`（X 的 `windows_live2d_sdk.c:212-213` 会误杀 `a..b` 且漏 UNC） |
| 符号链接 / 特殊文件 | 检查 zip 外部属性（Unix mode 高 16 位 `S_IFLNK`）与 Windows 属性位，**一律拒绝**，不解压 |
| 条目白名单 | 只接受匹配 `(^|/)Core/dll/(windows\|linux\|macos)/(x86_64\|x86\|arm64)/<corename>$` 的普通文件（大小写不敏感，`<corename>` 按平台）；其余条目**一个字节都不落盘** |
| 数量上限 | 条目总数 ≤ 20000（照 X `live2d_download.rs:50`） |
| 大小上限 | 单条目 ≤ 64 MiB（照 X `:13,60`）、白名单条目解压总量 ≤ 64 MiB、zip 文件本身 ≤ 512 MiB |
| 压缩比炸弹 | 边解压边累计，实际写入字节超过"声明尺寸 × 1.05"或超过上限即中止 |
| 路径长度 | 条目路径 ≤ 512 字节 |
| 中断/取消 | 支持取消（worker + 原子取消标志），取消即删 `.staging/` |

**④ 目录识别** — 在 zip 内定位 Core 根：优先 `<plat>/<arch>`；同一 zip 里多个候选按 `x86_64 > x86 > arm64`（本机优先）择一；找不到就明确报"这个包里没有适配本机（Windows x86_64）的 Core"，并列出包里实际存在的平台目录（照 X 的错误语义 `windows_live2d_sdk.c:416-420`：`No usable Cubism Core was found inside the SDK zip`）。
另外识别"裸文件"输入：用户直接选了 `Live2DCubismCore.dll` 时跳过解压，直接进校验。

**⑤ 校验与落盘**

1. magic：`MZ`（PE）/ `\x7FELF` / Mach-O（`0xFEEDFACF`/`0xFEEDFACE`/fat）；
2. 架构：PE Machine `0x8664/0x014c`、ELF `e_machine`、Mach-O `cputype` 必须与本机一致；
3. 导出符号：含 `csmGetVersion`、`csmReviveMocInPlace`、`csmInitializeModelInPlace`（用宿主 loader 的探测函数，照 X `windows_live2d_sdk.c:51-56`）；
4. 版本记录：`csmGetVersion()` 的**整数**值（踩坑见 `CHANGELOG.md:238`），写进 `core.json`；
5. 哈希：算 SHA-256 写进 `core.json` 并在 UI 显示（**不**与固定白名单比对，因为官方会更新版本；只用于用户核对与"同一文件重复导入"的短路）；
6. 落盘：先在 `<存储根>/live2d-core/.staging/` 写完 + `fsync` + 原子 rename 到 `<plat>/<arch>/`；
7. 升级/替换：旧文件改名为 `Live2DCubismCore.<旧sha8>.bak`（只保留最近一份），`core.json` 的 `previous` 字段记录，可一键回滚；
8. 失败回滚：任何一步失败 → 删 `.staging/`、保留旧 Core 与旧 `core.json`、UI 显示具体原因 + 写入 `state_root/live2d-core-import.log`；
9. 成功：宿主重新执行一次 loader 探测 → 拿到新句柄 → 通知渲染层重建（GL 重载模型；VK 需要重建离屏目标）→ toast `Live2D Core 已安装，已启用渲染`。

**⑥ 装载与热切换** — 宿主 `dlopen(RTLD_NOW|RTLD_LOCAL)`（照 X `posix_live2d_sdk.c:42`）或 `LoadLibraryW`；成功后旧句柄延迟到"当前模型销毁后"再 `dlclose`（避免正在使用的 csm 对象悬空）。X 的选择是"已加载就拒绝再次导入"（`windows_live2d_sdk.c:379-383`）；我们允许替换（用户明确要求升级/替换路径），但必须遵守"实例、GPU 资源先销毁再卸载"的规则（X 的同类注释：`model_plugin.h:36-38`）。

**⑦ 失败/回滚提示文案** 建议键名：`native.live2dCoreImport.{invalidMagic,archMismatch,missingSymbols,tooLarge,tooManyEntries,unsafeEntry,rollbackOk,installed,staged}`（10 语言）。

### 9.4 降级路径矩阵

| 状态 | 程序还能做什么 | 提示（键） | 入口 |
| --- | --- | --- | --- |
| 插件 + Core 都在 | 完整 Live2D（GL；VK 见 §9.7 阶段） | 无 | — |
| **缺 Core**，插件在且启用 | 模型列表/封面/导入/设置/按键音效/物品/统计/覆盖层全部正常；Live2D 模型不渲染（stub 兜底） | `native.live2dCoreMissing` + 引导页 | 设置窗口 toast 一次；插件页 `导入 Live2D Core` |
| **缺插件**（未安装或被 `.disabled`） | 同上；额外：`<exe>/plugins` 缺失时也不渲染 | `pages.preference.plugins.live2dMissing`（措辞指向"安装插件"，与缺 Core 区分） | 设置窗口 toast；插件页拖入/选择插件 + `打开插件目录` |
| 二者都缺 | 同上；提示两句合并（先装插件，再导入 Core） | 两条 toast | 同上 |
| **诊断构建**（我们自己出的无 SDK 构建，仅内部） | 启动与平台诊断 | `native.live2dSdkMissing`（照 X 的措辞）+ 保留包内 `cmake/DiagnosticBuildNotice.txt` | — |
| Core 在但架构/版本不对 | 同上 + 明确原因 | `native.live2dCoreImport.archMismatch` / `coreVersionUnsupported` | 重新导入 |

实现上**不需要新的降级代码路径**：我们的 `src/live2d/live2d_stub.c`（`:1-9` 的既有设计意图）就是"任何一样缺失时的实现"，只要保证它永远被编进宿主、并在 dispatch 层作为默认 ops 表即可。

### 9.5 宿主改造形态（照 X 的 dispatch 模式，最大化复用现有 347 个调用点）

```
宿主链接期固定包含：
  src/live2d/live2d_stub.c      → 提供一套"中性 ops"（现有文件，语义不变）
  src/live2d/live2d_dispatch.c  → 新增：bongo_cat_live2d_* 全部改成"判空转发 + 默认回落 stub"
  src/runtime/plugins/*         → 新增：插件查找/加载/ABI 校验/服务表
插件（plugins/bongo_live2d.*）：
  src/live2d/cubism_*.cpp + Framework（静态链进插件）+ 生成的 core shim
```

* 现有调用点（`bongo_cat_live2d_frame()` 之类）**签名不变**，因此 `src/render/*`、`src/runtime/*`、`src/ui/*` 的大部分代码零改动。
* 宏改名技巧可直接借用（X：`ModelPlugins.cmake:12-22` 把 `bongo_cat_model_runtime_<m>` 映射到 `stub_model_runtime_<m>`），我们对应地把 stub 的符号加 `stub_live2d_` 前缀，dispatch 层用原名。
* 现有 `include/bongo_cat/live2d_vulkan.h` 的 15 个入口在阶段 1 先保持"宿主内部实现"（VK Live2D 暂不插件化），阶段 3 再迁（见 §9.7）。

### 9.6 构建期守卫 + CI 检查（我们的版本）

**CMake 侧**

1. 新增 `option(BONGO_CAT_RUNTIME_CORE "Load user-supplied Cubism Core externally" ON)` + `if(NOT BONGO_CAT_RUNTIME_CORE) FATAL_ERROR "renderer plugins must not embed Cubism Core"`（照 X `CMakeLists.txt:11-16`），并**删除** `cmake/Cubism.cmake:11,29-48,90` 的 `Live2DCubismCore STATIC IMPORTED` 与 `target_link_libraries(Framework PUBLIC Live2DCubismCore …)`，改为 shim。
2. 新增 `cmake/GenCoreShim.cmake` + `cmake/gen_core_shim.py`（自行实现，注释里署名灵感来源）；输入只用 `<sdk>/Core/include/Live2DCubismCore.h`。
3. 宿主 target 不再链 `Framework`（改 `CMakeLists.txt:126-129`），Framework 只进 `bongo_live2d`（照 `ModelPlugins.cmake:46-50`）。
4. 新增 `cmake/CheckNoCoreInBinary.cmake`：Windows 用 `dumpbin /dependents`（或 `/imports`）断言宿主 exe 与插件的依赖表里没有 `Live2DCubismCore.dll`；Linux 用 `readelf -d` + `nm -u`；macOS 用 `otool -L` + `nm -u`。**复用我们已有的 `cmake/CheckWindowsImports.cmake` 模式**（它现在查 `SetWindowsHookEx`/RawInput，加一条 Core 断言即可，`cmake/CheckWindowsImports.cmake:9-19`）。
5. staging：新增 `cmake/ModelPlugins.cmake` 风格的 `bongo_cat_stage_renderer_plugins(target)`（目标目录 `<exe>/plugins`、Apple `Contents/PlugIns`、`copy_if_different` + 独立 custom target + 嵌套签名），并让 `install(FILES … DESTINATION plugins COMPONENT Runtime)`。
6. 着色器改内嵌进插件（照 `ModelPlugins.cmake:23-36` + `embed_plugin_shaders.py`），随后**删除** `cmake/CubismAssets.cmake:5-9,64-68` 的散文件安装与 `window.c:27-41` 的"缺 FrameworkShaders 就拒绝 Vulkan"分支（改为插件自报能力）。

**CI 侧**

| 位置 | 改动 |
| --- | --- |
| `.github/scripts/check-unix-package.ps1` | 新增：解包后递归扫文件名 `Live2DCubismCore.(dll\|so\|dylib)` → 命中即 `throw`（照 X `:28-33`）；再按**内容**扫一遍（magic + 字符串 `csmReviveMocInPlace`）以防改名夹带；新增：要求 `plugins/libbongo_live2d.so/.dylib` 存在且非空（照 X `:58`）；把 `:37-40` 的 `FrameworkShaders/*` 断言换成插件存在断言；smoke 改为"把 SDK 里的 Core 复制到 `<storage>/live2d-core/<plat>/<arch>/` 再跑"（照 X `:78-94`），并保留 `renderer=cubism-native` 断言（证明真的渲染了）与"缺 Core 也能启动"的第二条断言 |
| `.github/workflows/release.yml` | 删掉 `:159-161,174` 的 Core SHA-256 公示（改为 "Live2D 渲染：需用户自行导入 Cubism Core"）；`:144` 的 `-portable.exe` 改成目录包/自解压包并断言内含 `plugins/bongo_live2d.dll`；仿 X 加一条"`build-release` 下除 `vendor/` 外不得出现 `Live2DCubismCore.*`"（X `:467-474`），并额外扫 `dist/` 与安装器输出 |
| `.github/workflows/ci.yml` | 同样删掉 `:386-400` 的 Core SHA 块；Windows job 增加插件与"无 Core"断言 |
| `.github/scripts/check-publish-guards.ps1` | 照 X `:4-9,28-37,76-88` 加 Cubism 策略：markers（`restore-cubism-sdk`、`-DBONGO_CAT_REQUIRE_CUBISM=ON`、`-RequireCubism`、`vendor/CubismSdkForNative`、`cubism.live2d.com`、`CubismSdkForNative-`）+ 只允许 release.yml 还原 + 该 job 必须带 repo guard + `-SelfTest` 反例 |
| `.gitignore` / 仓库卫生 | 确认 `vendor/CubismSdkForNative/` 不入库；新增规则：任何 `Live2DCubismCore.*`、`*.moc3` 之外的 Core 变体不得提交（双保险） |
| 测试 | 新增 `tests/plugin/test_renderer_plugin_abi.c`（坏描述符矩阵：abi 版本错/`struct_size` 错/缺必需回调/谎报后端/`REQUIRES_CORE` 但无 Core → 断言被拒绝且宿主不崩，比 X 多这一层）；新增 `tests/live2d/test_core_import.c`（zip-slip、symlink、超限、白名单外条目、架构不匹配、失败回滚）；现有 `tests/live2d/*` 改为链 shim 或加载假 Core |

### 9.7 双后端（GL / VK）与插件边界 —— 关键决策

**现状**：GL 路径通过 `bongo_cat_live2d_*`；VK 路径通过 `bongo_cat_live2d_vulkan_*`，其中 `struct BongoCatVk *`（宿主私有）与裸 `VkImage/VkImageView/VkSampler` 直接穿过函数签名（`include/bongo_cat/live2d_vulkan.h:14,37,54,61-79`），合成侧再从 `target_info` 取裸句柄（`src/render/vk/vk_scene.c:36-46`）。

**方案 A（淘汰）**：只把 GL Live2D 放进插件、VK 留在宿主 → 宿主仍需链 Framework 与 Core ⇒ 违反第 1 条规则，等于没改。

**方案 B（终态，推荐）**：插件同时承载 GL + VK。宿主把 VK 的**能力**以服务表暴露（设备/队列/队列族/命令池/一次性命令缓冲 + `vkGetInstanceProcAddr`/`vkGetDeviceProcAddr`），`struct BongoCatVk` 绝不跨界；插件自持离屏渲染目标（image/view/sampler），只把**结果**（句柄 + 尺寸）交回宿主合成。这等于 X 的 `bongo_cat_rhi_get_vulkan_frame_info / begin_commands / submit_commands / wait_idle`（`model_plugin_host.h:25-29`）的轻量版。
代价：要改 `live2d_vulkan.h` 的 15 个入口 + `cubism_vulkan_bridge.cpp` / `cubism_vulkan_texture.cpp` / `cubism_vulkan_transition.cpp` + `vk_scene.c` 的合成路径。

**方案 C（分阶段，推荐落地顺序）**：

* 阶段 1：**只插件化 GL**。VK 后端在 Live2D 场景下**不启用 Live2D**（沿用我们现有"Vulkan 不行就回退 GL"的先例：`window.c:27-41` 会在缺着色器时明确失败并回退），VK 仍可跑覆盖层/桌面背景等其他功能。
* 阶段 2：稳定 ABI + 服务表后，再按方案 B 把 VK Live2D 迁进插件；此时 `vk_scene.c` 改为消费插件返回的离屏句柄。
* 好处：把最大的技术风险与"法律边界修复"解耦；先让"包内无 Core"成立（这才是必须尽快做的），VK Live2D 的成熟度问题留给我们自己的 VK 路线图（参考 `docs/vulkan-cubism-status.md`）。

### 9.8 我们自己的插件 ABI v1（参考六件套的具体化）

```c
/* include/bongo_cat/renderer_plugin.h —— 自行实现；设计参考 BongoCat-X（AGPL-3.0-only）
   的 include/bongo_cat/model_plugin.h，仅借鉴结构，不复制代码。 */
#define BONGO_CAT_RENDERER_PLUGIN_ABI 1u
#define BONGO_CAT_RENDERER_PLUGIN_SYMBOL "bongo_cat_renderer_plugin_query"

typedef struct BongoCatRendererPlugin {         /* ← 版本化描述符 */
    uint32_t abi_version;                       /* ← ② struct_size 搭档 */
    uint32_t struct_size;
    uint32_t engine;                            /* 1 = Live2D, 2 = Inox2D(预留) */
    uint32_t flags;                             /* REQUIRES_CORE | … */
    uint32_t graphics_backends;                 /* OPENGL | VULKAN */
    const char *name;
    BongoCatRendererOps ops;                    /* ← ③ 必需子集 + 可选回调 */
} BongoCatRendererPlugin;

typedef const BongoCatRendererPlugin *(*BongoCatRendererPluginQuery)(
    uint32_t abi, const BongoCatRendererHost *host);

/* ④ 可选扩展：各自独立导出，老宿主 LoadFunction 失败即忽略 */
#define BONGO_CAT_RENDERER_PLUGIN_VULKAN_FRAME_SYMBOL "bongo_cat_renderer_plugin_vulkan_frame_v1"
#define BONGO_CAT_RENDERER_PLUGIN_OPTIMIZE_SYMBOL     "bongo_cat_renderer_plugin_optimize_v1"
```

* **必需回调（8 个，照 X `model_plugins.c:84-85` 的同构集合）**：`create / destroy / load_ex / ready / update / draw_checked / resize / set_render_info`。
* **可选回调**：其余 `bongo_cat_live2d_*` 全部进 ops 表，判空调用（我们已有 `live2d_stub.c` 的中性返回语义可复用）。
* **⑤ 能力位协商**：`flags`（`REQUIRES_CORE`）、`graphics_backends`；加载顺序 = 描述符校验 → 后端支持校验 → Core 可用性校验 → `create`；优化开关走"请求 ∩ 支持 ∩ 实际返回"。
* **⑥ 宿主服务表**：`abi_version/struct_size` + `core_symbol` + 日志 + 内存 + 图像解码 + JSON + SHA-256 + 平台路径 + 对话框；规则：**插件不链接 SDL3-static、不自己开 CRT 边界**，所有分配用配套释放函数（照 X `model_plugin_host.h:17-18`）。
* **查找/安装/启停**：完全照 §6 补充那一套（env 覆盖、`<存储根>/plugins`、`<exe>/plugins`、`.disabled`、原子替换、活动插件禁改）。

---

## 10. 改动面清单

### 新增（建议路径）

| 文件 | 作用 |
| --- | --- |
| `include/bongo_cat/renderer_plugin.h` / `renderer_plugin_host.h` | 插件 ABI（描述符、ops、宿主服务表） |
| `include/bongo_cat/renderer_plugins.h` | 查找/安装/启停/信息查询 API |
| `src/runtime/plugins/renderer_plugin_paths.c` | `<存储根>/plugins` → `<exe>/plugins` → env 覆盖 → `.disabled` |
| `src/runtime/plugins/renderer_plugin_loader.c` | `SDL_LoadObject`、ABI/后端/Core 校验、生命周期、卸载顺序 |
| `src/runtime/plugins/renderer_plugin_services.c` | 宿主服务表实现（`core_symbol` 等） |
| `src/live2d/live2d_dispatch.c` | 现有 `bongo_cat_live2d_*` 的判空转发层（`live2d_stub.c` 为默认 ops） |
| `src/live2d/core_import.c` / `core_import.h` | 安全解压 + 目录识别 + 校验 + 原子落盘 + 回滚 + `core.json` |
| `src/platform/{windows,posix}/live2d_core_loader.c` | `LoadLibrary/dlopen` + 三符号探测 + `csmGetVersion` |
| `cmake/gen_core_shim.py` + `cmake/GenCoreShim.cmake` | 从官方头文件生成 `csm*` 转发 shim |
| `cmake/CheckNoCoreInBinary.cmake` | dumpbin/readelf/otool 断言 |
| `cmake/ModelPlugins.cmake`（我方版本） | 构建并 stage `bongo_live2d`、内嵌着色器、签名 |
| `src/ui/preferences/preferences_live2d_setup.c/.h` | 引导页（10 语言要点、链接、离线做法） |
| `src/ui/preferences/preferences_live2d_core_import.c` | 文件选择 + 后台导入 + 进度 + 结果 toast |
| `resources/assets/locales/*.json`（10 个） | 新增 `pages.preference.live2dSetup.*`、`native.live2dCoreMissing`、`native.live2dPluginMissing`、`native.live2dCoreImport.*` |
| `tests/plugin/test_renderer_plugin_abi.c` | 坏描述符矩阵 |
| `tests/live2d/test_core_import.c` | zip 攻击面 + 回滚 |
| `docs/live2d-core.md` | 用户向文档（怎么取得 Core、放哪、怎么验证、离线怎么做） |

### 修改

| 文件:line | 改动 |
| --- | --- |
| `CMakeLists.txt:44-82` | 宿主永远编 `live2d_stub.c`；真实现只进插件；`BONGO_CAT_CUBISM_ENABLED` 语义改为"是否构建插件" |
| `CMakeLists.txt:126-129` | 宿主不再 `target_link_libraries(... PUBLIC Framework)` |
| `cmake/Cubism.cmake:11,29-48,90` | 删除 `Live2DCubismCore` 静态导入；改用 shim；Framework 只进插件 |
| `cmake/CubismAssets.cmake:5-9,64-68` | 着色器不再进包/进 exe，改为内嵌插件 |
| `cmake/Packaging.cmake:13-17,43-55` | Windows 便携从"单文件 exe"改为"含 `plugins/` 的包"；诊断包名逻辑保留 |
| `cmake/PackagingInno.cmake` + `packaging/windows/BongoCat.iss.in` | 安装器要装 `plugins/` 子目录并纳入卸载 |
| `cmake/Dependencies.cmake:45-47` | Windows 打开 `SDL_DIALOG` |
| `src/runtime/lifecycle/storage_paths.c`（或插件模块内） | 解析 `<存储根>/plugins` 与 `<存储根>/live2d-core` |
| `src/runtime/lifecycle/{app.c,app_loop.c,shutdown.c}` | 插件装载时机（窗口/GL 上下文就绪后）、卸载顺序（先销毁实例与 GPU 资源） |
| `src/runtime/shell/window.c:27-41` | 缺着色器的 Vulkan 拒绝分支改为"插件能力"判定 |
| `src/render/vk/vk_scene.c:36-150` | 阶段 2：合成改消费插件返回的离屏句柄 |
| `include/bongo_cat/live2d_vulkan.h:37-79` | 阶段 2：`struct BongoCatVk *` 出口改为宿主服务 |
| `src/ui/preferences/preferences_model.c`、`preferences_pages.c`、`preferences_notice.h` | 新增 Core 导入入口与两种缺失的区分提示 |
| `src/runtime/model/*`（加载失败路径） | 区分"缺 Core"与"模型损坏"的错误文案 |
| `.github/scripts/check-unix-package.ps1:32-67` | 加 Core 断言 + 插件必需断言 + smoke 改为塞 Core 进 `live2d-core/` |
| `.github/scripts/check-publish-guards.ps1:6-23` | 加 Cubism markers 与"只允许 release.yml 还原 + fork guard" |
| `.github/workflows/release.yml:144,159-161,174`、`ci.yml:386-400` | 删 Core SHA 公示；portable 改包；加"dist 无 Core"断言 |
| `scripts/build-windows.ps1:159` | `-RequireCubism` 语义保留（CI 需要 SDK 才能编插件），本地默认仍可只编诊断宿主 |
| `tests/live2d/*`、`tests/ui/test_preferences_lifecycle.c` | 适配插件/导入流程 |

---

## 11. 工作量估计（乐观 / 保守）

单位：人日（含自测，不含跨平台真机验证与法务等待）。

| 阶段 | 内容 | 乐观 | 保守 |
| --- | --- | --- | --- |
| 0 | ABI 冻结 + 设计评审 + `AGENTS.md` 纪律 | 1 | 2 |
| 1 | 宿主改造：stub 常编 + dispatch + 插件加载器 + GL 插件能起来 | 4 | 8 |
| 2 | Core 安全导入链（解压/识别/校验/原子落盘/回滚）+ 引导页 + 10 语言 + `SDL_DIALOG` | 4 | 9 |
| 3 | VK 边界（服务表 + 离屏所有权 + 合成改造 + 回退路径） | 5 | 14 |
| 4 | 构建守卫 + CI 断言 + 打包形态改造（单 exe → 目录包/自解压 + 安装器） | 2 | 5 |
| 5 | 测试（ABI 反例、zip 攻击面、端到端导入、GL/VK 回归） | 3 | 7 |
| 6 | 文档（README 10 语言 + `docs/live2d-core.md` + 发布说明） | 1 | 3 |
| **合计** | | **≈20** | **≈48** |

* 若**砍掉阶段 3**（先只插件化 GL，VK 下 Live2D 暂不可用）：**乐观 ≈12 / 保守 ≈26**。建议先走这条，把"包内无 Core"尽快落地。
* 关键路径是阶段 1+2+4（法律边界与用户流程），阶段 3 可以并行延后。
* 额外不确定项：Core 版本兼容矩阵（`5-r.5`/`5-r.6`…）与 macOS 公证/签名流程的额外往返，各留 2-3 天缓冲。

---

## 12. 对用户的影响 & 发布说明怎么写

### 12.1 影响（用户看得见的）

1. **不再是单文件 exe**：Windows 便携版从 `BongoCat-<ver>-windows-x64-portable.exe`（`cmake/Packaging.cmake:44-52`）变成压缩包（exe + `plugins/` + 其他 DLL）。老用户"只把 exe 拷到桌面就跑"的习惯会**失去 Live2D 渲染**（普通 2D/覆盖层不受影响）。安装器版本不受影响，但安装目录会多出 `plugins/`，卸载要一起清。
2. **老用户升级后，现有 Live2D 模型不会失效，但会暂时不渲染**：模型、封面、设置都在 `data/models`、`config` 树里（`storage_paths.c:51-58`），与本次改动无关；第一次打开设置会看到"未检测到 Live2D Cubism Core"的提示与导入入口。**不会**丢数据、**不会**丢模型选择，只是渲染为空直到导入 Core。
3. **离线用户**：无法完成导入 → 无 Live2D 渲染（其余功能照常，包括 Inochi2D/Mver/图片模型，如果该机器上本来就用这些）。救济办法：在别的机器下载官方 SDK 后，把 zip 或 `live2d-core/` 目录整体拷过来（文档里要写清楚路径）。
4. **首次启用需要一次联网**（去官网拿 SDK）；我们**不代替用户下载**（v1 决策）。
5. **多一个可被安全软件关注的 DLL**：从用户目录加载 Core 可能触发启发式告警；UI 会显示 SHA-256 供核对。
6. 便携模式用户：`BongoCatData/` 里会多出 `plugins/` 与 `live2d-core/`，整体拷贝仍然可用。

### 12.2 发布说明写法（按你的偏好：只写用户看得见的行为，每项带作者）

模板（放在 release notes 的"用户可见变化"小节，不写实现细节，不写"我们不再静态链接 Core"这类话）：

```
### 用户可见的变化

- **作者：<你的署名>** ｜ Live2D 渲染现在由独立插件提供：安装包不再包含 Live2D 运行库，
  首次想在桌宠上看到 Live2D 模型时，需要在「设置 → 插件」里导入你自己的 Cubism Core
  （我们不分发它，需要你自行同意 Live2D 的许可协议）。
- **作者：<你的署名>** ｜ 新增「导入 Live2D Core」向导：选择官方 SDK 压缩包或 Core 文件即可，
  程序只从中取出你系统对应的 Core，校验通过后立刻生效；导入失败会保留原来的 Core 不变。
- **作者：<你的署名>** ｜ Windows 便携版由单个 exe 改为压缩包：解压后请保留 plugins 文件夹，
  只把 exe 单独拷走将无法使用 Live2D（其他功能不受影响）。
- **作者：<你的署名>** ｜ 已有的模型、封面和设置都不会变；升级后第一次打开设置会看到一次提示，
  按提示导入 Core 即可恢复 Live2D 渲染。
```

写作纪律：① 每条只描述"用户看到/需要做什么"，不提 shim、静态链接、ABI；② 明确"我们不分发 Core"；③ 给出恢复路径（导入 Core）而不是只报"功能没了"；④ 离线救济写进 README/文档，发布说明里一句话带过。

---

## 13. 风险

| 风险 | 说明 | 缓解 |
| --- | --- | --- |
| **插件崩溃隔离为零** | `dlopen` 同进程，插件或用户提供的 Core 崩溃 = 主进程崩溃（X 同样没有隔离，`AGENTS.md:44-46` 只约束链接） | ① 严格 ABI/版本校验；② 启动参数 `--no-renderer-plugin`（安全模式）；③ 检测上次异常退出（`state_root/startup-stage.txt` 之类已有机制）后提示"以安全模式启动"；④ 崩溃报告只写日志不自动上报 |
| **ABI 维护成本** | 我们的 Live2D API 面比 X 更大：67 个符号、含 `BongoCatLive2DRenderOptions`/`BongoCatLive2DVisualState`/`BongoCatLive2DFrame`/`BongoCatParameterRange` 等结构体（`include/bongo_cat/model.h:108-147`），结构体一改就要 bump | ① 所有跨界结构体带 `size` 字段或整体走 `struct_size`；② 可选回调一律判空；③ 新增能力只加新符号不加字段；④ ABI 反例单测进 CI |
| **双后端共存** | `include/bongo_cat/live2d_vulkan.h` 把宿主私有 `struct BongoCatVk *` 与裸 `VkImage/View/Sampler` 穿过边界（`:14,37,54,61-79`），`vk_scene.c:36-150` 依赖它；这些**不能**跨 DLL | 按 §9.7 的 C→B 分阶段；阶段 1 VK 下不启用 Live2D 并沿用现有回退先例（`window.c:27-41`）；阶段 2 引入 VK 服务表 + 插件自持离屏目标 |
| **Core 版本/架构不匹配** | 用户可能拿到旧版、ARM、x86 或 Linux 版文件 | 导入即校验 magic/架构/三符号/`csmGetVersion`，失败保留旧 Core 并给具体原因；`core.json` 记录版本便于支持排查 |
| **法律解读** | "帮用户导入"仍可能被解读为协助取得/使用；应用内代下载（X 的做法）更进一步 | v1 只做**本地文件只读导入**：不联网、不代下载、不镜像、不缓存到发行目录；README + 引导页 + 发布说明三处明示"我们不附带、不分发"；不做 SDK 文件的再分发 |
| **安全软件误报** | 从用户目录 `LoadLibrary` 新出现的 DLL | 只从固定目录加载、显示 SHA-256、不静默替换；安装器/便携包继续签名 |
| **体验倒退** | 首次使用多两步，且必须联网一次 | 引导页讲清楚 + 一键打开官网 + 支持"我已经下好了"直选文件 + 拖放 zip 到设置窗口（我们已有拖放机制：`preferences.c:177,334`） |
| **CI 自证风险** | 我们当前在 release notes 里公示 Core SHA-256（`release.yml:159-161,174`） | 本次一并删除；新增"dist 无 Core"断言，避免回归 |
| **诊断构建与正式构建混淆** | `BONGO_CAT_CUBISM_ENABLED` 语义改变后，包名/提示要能区分 | 保留 `BONGO_CAT_PACKAGE_PRODUCT`/`DiagnosticBuildNotice.txt` 机制（`Packaging.cmake:13-17`），并让 UI 用 `live2dSdkMissing` 区分"本构建不带" vs "缺 Core" |

---

## 14. 替代方案排序

| 排序 | 方案 | 代价 | 法律含义 |
| --- | --- | --- | --- |
| **1（推荐）** | **随包发我们编译的插件 + 用户本地导入 Core**（本方案） | 多文件包、首次两步、VK 边界改造（≈12-48 人日） | 边界清晰：发行物只含我们自己编译的 Framework 链接产物 + 我们自己写的 shim；Core 由用户按自己的许可协议从官方取得；我们不分发、不代下载 |
| 2 | 只支持"用户自编译插件"（不随包发插件，像 X 的源码构建路径） | 普通用户不可用（要 VS2022 + CMake，前一轮已确认用户机器没编译器） | 最干净（我们不含任何 Cubism 编译产物），但等于放弃 Live2D 功能 |
| 3 | 放弃 Cubism，改用开源替代（Inochi2D/Inox2D、Mver 等）作为**补充**渲染路径 | 不能跑 `.moc3` 模型，用户已有模型全部不可用 | 零风险；建议作为"不导入 Core 时的可选渲染路径"卖点保留，而不是替代方案 |
| 4 | 应用内代下载官方 SDK（X 的做法：同意协议后由程序下载并只取 Core） | 体验最好；实现成本中等（我们已有 HTTP + 哈希基础设施） | 最激进：把"取得 Core"的动作搬进我们的程序，法律上更接近协助取得；若采用必须：只走官方 HTTPS、显示两份协议、显式点击同意、不落盘到发行目录、不做镜像、不留缓存副本。**建议 v1 不做，留作 v2 选项并先做法务确认** |
| 5（现状，必须淘汰） | 继续把 Core 静态链进 exe | 技术上最省事（单文件 exe） | 风险最高：发行包含专有库的目标码；Live2D 许可明确限制再分发；且我们还在发布说明里公示了它的哈希 |

结论：**先做排序 1 的阶段 1+2+4**（宿主 + GL 插件 + 导入链 + CI 守卫），把法律边界落地；阶段 3（VK 插件化）随后；排序 3 作为长期补充路线；排序 4 需要法务确认后再评估。

---

## 附录 A：证据索引

### A.1 X 的关键 commit（均为 HEAD `0d0429d` 的祖先）

| commit | 日期 | 主题 | 引入的关键机制 |
| --- | --- | --- | --- |
| `ff1f84370986be7a221fbce30394144421f47ec1` | 2026-10-09 | `feat: dynamic renderer plugins with native Inox2D backend` | `CMakeLists.txt` 的 `BONGO_CAT_RUNTIME_CORE` FATAL_ERROR、`Cubism.cmake:34` 的 "Core is never linked" 注释、宿主服务表 `core_symbol`、`model_plugin_loader.c` 的 `REQUIRES_CORE` 校验、`model_plugins.c` 的 `<data>/plugins` 与 env 覆盖 |
| `c364dec4106bb7941bb3bea496a2994341d4cc0d` | 2026-10-01 | `feat(windows): import Live2D Cubism Core at runtime` | `BONGO_CAT_RUNTIME_CORE` 选项、Windows 运行期 Core 导入链路 |
| `2e630cdd9fc166a82c21538634528bad922336ed` | 2026-10-01 | `feat(platform): extend runtime-Core Live2D import to Linux and macOS` | `cmake/Cubism.cmake` 引入 `gen_core_shim.py`；POSIX loader |
| `47560c85e78e2919579373dbefdb5ca5cb217112` | 2026-10-01 | `ci(release): build runtime-Core packages from the official SDK download` | `check-unix-package.ps1` 的 "Core binary leaked into package" 断言 |
| `0478b94126e36d64eefd7a087d69c7b9df131eff` | 2026-10-01 | `ci: ship SDK-free builds from the test branch and harden publish guards` | `check-publish-guards.ps1` 的 Cubism SDK 策略 |
| `962f070bb9dac194a28d784d65629302675291fb` | 2026-10-10 | `feat: release 2.1.1 with renderer optimizations and rolling nightlies` | `bongo_cat_model_plugin_optimize_v1` 可选扩展符号 |
| `0bfecbadf07c08456a2b2ef73c1d579375f3dff3` | 2026-10-10 | `feat: add image backgrounds and renderer optimizations` | `bongo_cat_model_plugin_vulkan_features_v1` 可选扩展符号 |
| `2a63f330df5e11a5d1f3c2a49568faae48b30838` | 2026-10-14 | `feat: release BongoCat-X 2.2.2` | 10 语言的 `pages.preference.live2dSetup.*` 文案、`src/rust/bongo-safe/src/live2d_download.rs`（应用内下载官方 SDK 并只取 Core） |

复现命令（在 `%TEMP%\bcatx\BongoCat-X`）：

```powershell
git log --oneline -n 3 -S "Core is never linked into the plugin" -- cmake/Cubism.cmake      # → ff1f843
git log --oneline -n 3 -S "must not embed Cubism Core" -- CMakeLists.txt                    # → ff1f843
git log --oneline -n 3 -S "Core binary leaked into package" -- .github/scripts/check-unix-package.ps1  # → 47560c8
git log --oneline -n 3 -S "gen_core_shim" -- cmake/Cubism.cmake                             # → 2e630cd
git log --oneline -n 3 -S "live2dSetup" -- resources/assets/locales/en-US.json              # → 2a63f33
```

### A.2 X 侧证据速查

* 许可/同意：`README.md:16,19,51-63,118-150`、`docs/README.en-US.md:66-77,491`、`resources/assets/models/LICENSE:10`、`AGENTS.md:36-47`、`preferences_live2d_setup.c:24-47,82-145`、`preferences_live2d_setup_dialog.c:30-109`、`preferences.c:107-127`、`live2d_download.rs:12,45-98,100-149`、`preferences_plugins.c:42-44,75-76,85-93`
* 构建守卫：`CMakeLists.txt:8-16,104-128,188-191,198-204,229,240,276`、`cmake/Cubism.cmake:24-34,35-45,47-59,63-64,79,99-103,117`、`cmake/gen_core_shim.py:2-14,30-35,68-74`、`cmake/ModelPlugins.cmake:2,4-11,12-22,23-36,37-39,46-58,64-65,118-155`
* CI/包体：`.github/scripts/check-unix-package.ps1:28-33,49-58,72-73,78-94`、`.github/workflows/release.yml:1-8,304-306,337-345,467-474,506-514,524-528`、`.github/scripts/check-publish-guards.ps1:1-9,21-37,47-55,64-88,127-147`、`.github/scripts/restore-cubism-sdk.ps1:3-22,50-57`
* 降级：`model_plugin_loader.c:12-44,53-70,81-87`、`model_plugins.c:16-50,62-80`、`live2d_stub.c:1-22`、`live2d_dispatch.c:3-155`、`preferences_lifecycle.c:152-163`、`resources/assets/locales/en-US.json:469-470`、`preferences_plugins.c:58-66,106-107,137`、`CMakeLists.txt:104-128`
* ABI 六件套：`include/bongo_cat/model_plugin.h:9-10,11-25,32-35,36-38,39-86,88-94`、`include/bongo_cat/model_plugin_host.h:17-21,22-99`、`src/live2d/cubism_plugin.cpp:19-31,32-36,37-53,55-71`、`src/runtime/model/plugins/model_plugin_services.c:15-17`、`docs/model-plugins.md:3-17,19-33,35-39`

### A.3 我方证据速查

* 现状问题：`cmake/Cubism.cmake:11,29-48,90`、`CMakeLists.txt:8,11,44-82,126-129`、`cmake/Packaging.cmake:13-17,43-55`、`release.yml:144,159-161,174`、`ci.yml:386-400`、`cmake/Dependencies.cmake:45-47`、`cmake/CubismAssets.cmake:5-9,38-68`、`check-unix-package.ps1:32-67`、`check-publish-guards.ps1:6-23`
* 可复用的基础设施：`src/runtime/lifecycle/storage_paths.c:1-13,51-58,70-79`、`src/runtime/lifecycle/portable_mode.c:33-63`、`include/bongo_cat/common.h:14,27,34`、`src/ui/preferences/preferences_notice.h`（toast）、`src/ui/preferences/preferences.c:177,334`（拖放）、`src/live2d/live2d_stub.c:1-22`、`include/bongo_cat/model.h:99-253`、`src/render/vk/vk_scene.c:36-150`、`src/runtime/shell/window.c:20-41`、`cmake/CheckWindowsImports.cmake:1-20`（产物审计模式）

---

## 附录 B：未确认项 / 不要照抄清单

**未确认（X 侧无证据，不要当成事实）**

1. X 的**发行物**里是否真的一个 Core 文件都没有 —— 只能证明 CI 有断言（`check-unix-package.ps1:28-33`、`release.yml:467-474`），我没有下载任何 release 资产去核对（只读约束，且无必要）。
2. `cmake/Tests.cmake:471-477` 提到的 Windows "delay-load the Core DLL import" —— 全树 grep `delayload/DELAYLOAD/Live2DCubismCore.lib/IMPORTED_IMPLIB` **零命中**，实际机制是生成的 shim。该注释疑似过时，**不要照抄**。
3. X 是否有独立的法律意见/法务审核记录（仓库内没有相关文件）。
4. X 的 `mac-app-store.yml` 里 SDK 还原的具体合规口径（我只确认了守卫策略允许它还原，没逐行读该工作流）。
5. 官方 Cubism SDK 是否允许"只提取并再分发 Core 的哈希/元数据"这类做法 —— 未查（不属于本次只读调研范围，建议由法务确认）。
6. X 各类 Core 版本（`5-r.5` 之外的版本）的兼容矩阵 —— X 只把 `5-r.5` 的路径写死在下载器里（`live2d_download.rs:53-56`），是否兼容更新版本未确认。

**不要照抄的清单**

1. 不要照抄 `extract_sdk_zip` 的"全量解压 + `strstr("..")`"做法（`windows_live2d_sdk.c:198-251`）：改成条目白名单 + 按分量判定 + 拒绝符号链接。
2. 不要照抄"不校验下载物哈希"（`live2d_download.rs`）：我们至少记录 SHA-256 并在 UI 显示。
3. 不要照抄 `Tests.cmake:471-477` 的 delay-load 注释（与实现不符）。
4. 不要复制 X 的二进制或源码片段。X 是 AGPL-3.0-only（`README.md:19`、`resources/assets/models/LICENSE:10`）：借鉴设计要署名，搬运代码要同源许可。本方案的默认立场是**自行实现**。
5. 不要照抄 X 的"Core 放 `<data>/plugins` 附近"的直觉 —— X 恰恰是把 Core 挡在插件管理目录之外（`model_plugin_archive.c:60-61`、`test_plugin_archives.py:45`）；我们明确放 `<存储根>/live2d-core/`。
6. 不要照抄 X 的 `Cancelling keeps the plugin enabled without rendering` 之外的失败语义 —— 我们允许替换/回滚 Core，卸载前必须销毁实例与 GPU 资源（X 的同类约束见 `model_plugin.h:36-38`）。

---

*报告完。本文件由只读调研生成，未修改仓库任何其他文件，未提交，未发布。*

---

# 施工计划（本轮追加，阶段 0 产出）

> 本章节由**施工轮**追加，不是只读调研。它把上面的方案固化成"可执行、可验收、可分阶段提交"的施工计划，
> 并**修正**方案里与用户明确要求冲突的一处结论（见 §P0.2）。追加章节不覆盖上面的任何原文。

## P0.1 与用户三条硬要求的对齐

| 用户要求 | 方案原文 | 施工计划的口径 |
| --- | --- | --- |
| ① 发行包**不得**含 Cubism Core，改为"引导用户去官网 → 选压缩包 → 安全解压 → 配置 Core" | 与 §9.1/§9.3 一致 | 直接采用；**阶段 1 必须落地**（法律刚需） |
| ② **GL 与 VK 两个后端都插件化** | §9.7 方案 C 建议"阶段 1 只插件化 GL，VK 下 Live2D 暂不可用" | **该终态被否决**：VK 必须一起做完（阶段 2 的范围包含 VK）。方案 C 只作为**实施顺序**保留：先让 GL 路径在插件架构下跑通，再把 VK 路径迁进来，但两者都必须在本次任务范围内关闭 |
| ③ 兼容 `TianMengLucky/BongoCat-X` 的插件 | 方案未评估 | 见 §P0.5 的可行性判决：**A（完整二进制兼容）不可行 / 代价不可接受**；V1 只做 **D（描述符发现 + 精确报错）**，并保留 C 的条件 |

## P0.2 对方案 §9.7 的正式修正

原文结论（`§9.7` 方案 C）："阶段 1：只插件化 GL；VK 后端在 Live2D 场景下不启用 Live2D"。

**修正**：这只是**施工顺序**，不是终态。理由：法律边界（包内无 Core）要求**宿主与插件都不含 Core**；
只要 VK 的 Live2D 实现还留在宿主里，宿主就必须继续链 Framework 与 Core，第 1 条不可越界的规则就不成立，
"包内无 Core"就无法达成。因此 VK 路径**必须**在阶段 2 内完成迁移，否则阶段 1 的合规成果会被 VK 路径推翻。

修正后的实施顺序（都在本任务范围内）：

1. 阶段 2a：插件 ABI + 宿主 loader/dispatch + 服务表（含 VK 能力位）；
2. 阶段 2b：**GL** 路径在插件内跑通（`cubism_renderer_factory` 的 GL 分支）；
3. 阶段 2c：**VK** 路径在插件内跑通（插件自持离屏目标 image/view/sampler + 宿主服务表提供设备/队列/命令生命周期，
   `struct BongoCatVk *` 与裸 `VkImage/View/Sampler` 不再穿越 DLL 边界）；
4. 阶段 2d：宿主删除 `Framework`/Core 链接，改由 shim 持有 `csm*` ABI。

## P0.3 阶段划分与任务清单

### 阶段 0：计划落地（本文件本章）

任务：
1. 把阶段划分、每阶段任务清单、验收标准、风险写进本文件（追加，不覆盖）；
2. 给出 X 插件兼容性的可行性结论与代价（§P0.5）；
3. 明确"只编 `build-vulkan`、warnings-as-errors、每阶段 0 error"的验证口径（§P0.4）。

**不动核心构建**（不碰 `CMakeLists.txt` 的链接结构、不碰 `cmake/Cubism.cmake`）。

### 阶段 1：合规刚需 + 基础设施（法律边界先行）

| # | 任务 | 落点 |
| --- | --- | --- |
| 1.1 | 删除发行流程里公开 Core SHA-256 的地方（自证分发） | `.github/workflows/release.yml`、`.github/workflows/ci.yml` |
| 1.2 | 打开 Windows 的 `SDL_DIALOG`（文件选择器前置能力） | `cmake/Dependencies.cmake:45-47` |
| 1.3 | 构建守卫 + CI/包体检查：发行配置不得把 Core 链进宿主/插件；发行包**无** `Live2DCubismCore.*`、**必须**有 Live2D 插件 | `CMakeLists.txt`、`cmake/`、`.github/scripts/` |
| 1.4 | 插件发现与目录约定（env 覆盖 → `<存储根>/plugins` → `<exe>/plugins` → 兼容读取）+ 启动扫描 + 日志 | `src/runtime/plugins/`、`include/bongo_cat/` |
| 1.5 | Core 落点与用户流程：`<存储根>/live2d-core/<plat>/<arch>/` + 引导页（10 语言）+ 文件选择 + **安全解压** + 目录识别 + 原子落盘 + 失败回滚 + `core.json` | `src/live2d/core_import.c`、`src/platform/*/live2d_core_loader.c`、`src/ui/preferences/`、locale |
| 1.6 | 降级路径：缺插件/缺 Core 照常启动（`live2d_stub.c` 兜底）+ 设置里一次性提示（10 语言） | `src/runtime/lifecycle/`、`src/ui/preferences/`、locale |

### 阶段 2：把 Live2D 做成我们自己的插件（GL + VK）

| # | 任务 |
| --- | --- |
| 2a | `include/bongo_cat/renderer_plugin.h` + `renderer_plugin_host.h`（版本化描述符 + `struct_size` + 必需 ops 子集 + 能力位 + 宿主服务表含 `core_symbol`） |
| 2b | 宿主：`renderer_plugin_loader.c`（ABI/后端/Core 校验、`.disabled`、原子安装）、`renderer_plugin_services.c`（服务表实现）、`live2d_dispatch.c`（`bongo_cat_live2d_*` 全部改判空转发，默认回落 stub） |
| 2c | 插件：现有 Live2D 实现（Framework + 桥接 + GL/VK 渲染器）编成 `plugins/bongo_live2d.dll`，**插件内不含 Core**（`csm*` 经服务表 `core_symbol` 解析，或链生成的 shim） |
| 2d | VK 边界：服务表暴露设备/队列/队列族/命令池/一次性命令缓冲 + `vkGet*ProcAddr`；插件自持离屏 image/view/sampler，只把结果（句柄 + 尺寸）交回宿主合成 |
| 2e | 宿主删除 `Framework` 与 `Live2DCubismCore` 链接（`cmake/Cubism.cmake:11,29-48,90`、`CMakeLists.txt:126-129`）；着色器内嵌进插件 |
| 2f | 打包形态：单文件 exe → 含 `plugins/` 的包（`cmake/Packaging.cmake`、`cmake/PackagingInno.cmake`、`packaging/windows/BongoCat.iss.in`） |

### 阶段 3：X 插件兼容（按 §P0.5 的判决执行）

任务：实现**描述符发现 + 精确报错**（选项 D）：
识别 `<plugins>/bongo_live2d.dll` / `bongo_inox2d.dll` 一类 BongoCat-X 插件（按其导出符号
`bongo_cat_model_plugin_query`、`bongo_cat_model_plugin_optimize_v1`、`bongo_cat_model_plugin_vulkan_features_v1`），
**不调用**它的 query（避免用假宿主表把对方带崩），拒绝加载并给出可操作提示 + 指向官方 X 发行版。

## P0.4 验收标准（每阶段必须全绿才算完成）

统一验证口径（本机）：

```powershell
cmake -S . -B build-vulkan                      # 新增文件后先重新配置
cmake --build build-vulkan --config Release --target bongo_cat <新增测试目标> /m
ctest --test-dir build-vulkan -C Release        # 不得回退（windows-capture 在 HEAD 上就红，不算回退）
cmake --build build-vulkan --target check-lines check-localization
```

| 阶段 | 验收标准 |
| --- | --- |
| 0 | 本文件追加章节可读、无自相矛盾；X 兼容结论有 `文件:line` 证据；**未改动任何构建文件**；提交并推送 |
| 1 | ① `release.yml`/`ci.yml` 里**零** Core SHA-256 公示；② Windows 构建里 `SDL_DIALOG` 为 ON，`SDL_ShowOpenFileDialog` 可链接；③ 发行配置把 `BONGO_CAT_RUNTIME_CORE=OFF` 直接 `FATAL_ERROR`；包体检查脚本对"含 Core"的假包**必须失败**、对"缺插件"的假包**必须失败**（用脚本自检/反例验证）；④ 插件目录解析顺序与日志有单测；⑤ 安全解压对 zip-slip / 符号链接 / 超限 / 白名单外条目**逐条有反例测试**，且失败后旧 Core 与 `core.json` 不变；⑥ 缺 Core 与缺插件两种状态下程序都能启动到主界面（有测试或 smoke 断言）；⑦ `check-lines`/`check-localization`/`ctest -C Release` 不回退；⑧ 提交并推送 |
| 2 | ① 宿主 exe 与插件的导入表里**没有** `Live2DCubismCore.*`（`dumpbin /dependents` 断言进构建）；② `plugins/bongo_live2d.dll` 由我们构建并被 stage；③ **没有 Core 的机器上**程序照常启动、模型列表可用、只提示不可渲染；④ **导入 Core 后**GL 与 VK 两个后端都能渲染既有模型；⑤ 提交并推送 |
| 3 | ① 把 X 的 `bongo_live2d.dll` 丢进插件目录 → 程序**不崩**、拒绝加载、日志与 UI 给出明确原因；② 提交并推送 |

## P0.5 X 插件兼容性可行性结论（阶段 3 的依据）

> 证据来自 `%TEMP%\bcatx\BongoCat-X`（HEAD `0d0429d`）与本站仓库，均为只读。

### 结论（一句话）

**完整二进制兼容（选项 A）不可行、也不值得做**：X 的插件是拿 X 的宿主头文件编译出来的，
它不仅要求我们**逐字节复刻** X 的私有结构体布局，还要求我们**实现 X 的 GL 纹理管线与 Rust `bongo-safe` ABI 语义**；
而它换来的能力可以被我们自己 90 行代码的"描述符发现 + 明确拒绝"替代掉 90% 的价值。
**V1 只做选项 D**；选项 C 留作条件方案（见下）。

### 为什么 A 不可行（三条硬证据）

1. **跨界类型是 X 的私有结构体，不是 Vulkan/GL 的公共类型**：
   `BongoCatModelPluginHost`（`include/bongo_cat/model_plugin_host.h:19-99`）有约 90 个服务，其中
   `device_info(BongoCatRhiDeviceInfo*)`（`:24`）、`vulkan_frame(const BongoCatRhi*, BongoCatRhiVulkanFrameInfo*)`（`:25`）、
   `bongo_cat_image_texture_model(...)`（`:57`）、`bongo_json_read(...)`（`:97`）、`bongo_safe_image_decode(...)`（`:78`）
   的参数/返回值全是 X 自己的类型（`rhi.h:56-66,102-131`、`image.h`、`json_dom.h`、`safe_ffi.h`）。
   插件二进制里**固化**了这些结构体的字段偏移 —— 复刻必须逐字节正确，否则插件会在我们进程里按错误偏移读写。
   注意：**这一条与我们自己的问题正交** —— X 的 Vulkan 侧跨界的是 `void*` 句柄（`rhi.h:105-118`：`vulkan_device`、`vulkan_queue`、
   `vulkan_command_pool`、`image/view`），属于可复刻的 POD；难的不是句柄，是那 90 个服务的语义。
2. **Live2D 插件的 GL 路径依赖宿主创建纹理并维护缓存/alpha mask/缩放语义**：
   插件侧把宿主服务直接宏展开成全局函数（`src/live2d/cubism_plugin_services.hpp:116-123`），
   Live2D 插件里 `bongo_cat_image_texture_model` 出现 10 次、`..._scaled_cached` 5 次、
   `bongo_cat_image_make_alpha_mask` 6 次、`bongo_cat_image_resize_rgba_take` 6 次、`bongo_safe_image_decode` 6 次、
   `bongo_json_read` 6 次（全树 grep 计数）。也就是说"渲染正确"取决于我们**等价重写 X 的图像/纹理/JSON 管线**，
   而不是"把句柄转过去"。
3. **插件还要求宿主的 RHI 逐帧配合**：`bongo_cat_rhi_get_vulkan_frame_info` 5 次、`bongo_cat_rhi_begin_commands` 4 次
   （同上计数）；渲染命令的生命周期（begin → 录制 → submit → wait_idle）由宿主拥有（`model_plugin_host.h:27-29`）。
   我们必须把自己的 VK 帧循环改造成"能被外来插件借用"的形态，这与阶段 2d 要做的改造**部分重叠但不等价**
   （阶段 2d 只服务我们自己的插件，不需要兼容第三方对帧序的假设）。

### 选项代价对照

| 选项 | 内容 | 代价 | 判决 |
| --- | --- | --- | --- |
| **A** | 加载并渲染 X 原生 `bongo_live2d.dll` | 复刻约 90 个服务 + X 的 GL 纹理/JSON/图像管线语义 + Rust `bongo-safe` ABI + 逐字节结构体布局；乐观 **25 人日**、保守 **45 人日**，且无上游稳定性承诺（X 改 `struct_size` 即全部失效），需要 X 自己的测试集才能验证"渲染正确" | **不做** |
| **B** | 只加载 + 协商，不做渲染 | 需要 A 的结构体复刻 + `REQUIRES_CORE` 协商（`src/runtime/model/plugins/model_plugin_loader.c:25-44`）才能过 `query`；且"能加载不能渲染"对用户毫无价值 | **不做** |
| **C** | 只兼容"自带渲染器"的插件（如 Inox2D） | Inox2D 插件**不是**自包含的：它要 `gl_proc` 非空（`src/rust/bongo-inox2d/src/lib.rs:108-115`）、要 `device_info`/`vulkan_frame`（`native.rs`：`native_vulkan.rs:3` 注释"recorded directly on the host Vulkan queue"）、要 `set_rhi_info`（`ffi.rs:66`）。不过它的 Rust 侧只声明了宿主表的**稳定前缀**（`src/rust/bongo-inox2d/src/abi.rs:114` 注释 `Only this stable prefix is consumed`），比 A 小。乐观 **10 人日**、保守 **20 人日** | 条件方案：仅当"我们能提供与 X 等价的 Vulkan RHI 借用契约"在阶段 2d 顺手成立时才评估，否则不做 |
| **D** | 只做**描述符发现 + 精确报错** | **0.5-1 人日**：`SDL_LoadObject` 后只 `LoadFunction` 找 `bongo_cat_model_plugin_query` 等导出符号，**不调用** `query`（X 的实现要求 `host` 非空且 `struct_size` 完全一致，见 `src/live2d/cubism_plugin.cpp:55-58`；传假宿主表有把第三方插件带崩的风险），命中即拒绝加载并给可操作提示 | **本任务做这个** |
| **E** | 什么都不做 | 0 | 会被用户当作"不支持 X 插件生态"，且用户明确要求"顺便兼容" |

### 许可与署名（若将来做 C）

X 是 **AGPL-3.0-only**（`README.md:19`）。若复用其头文件/源码片段（含 ABI 结构体定义），
我们的对应文件必须同源许可并保留署名与 commit 号；**本任务不搬运任何 X 代码**，只按 §P0.5 的证据自行实现"符号发现"，
因此不产生 AGPL 传染。将来做 C 时，落点文件头部需写明：
`设计参考 TianMengLucky/BongoCat-X (AGPL-3.0-only), commit 0d0429defa0876c33bd7bfc0d0cd71bba16b02d7`。

## P0.6 风险（施工增量，补充 §13）

| 风险 | 说明 | 缓解 |
| --- | --- | --- |
| **VK 插件化是最大技术风险** | `include/bongo_cat/live2d_vulkan.h:37-79` 把宿主私有 `BongoCatVk*` 与裸 `VkImage/View/Sampler` 穿过 API；`src/render/vk/vk_scene.c:36-150` 依赖它 | 阶段 2a/2d 先把服务表与离屏所有权设计定型；GL 先跑通以冻结 ABI；VK 迁移期间保留"VK 下不启用 Live2D"的**临时**回退（必须在本任务内删掉） |
| **warnings-as-errors 下的第三方头污染** | 打开 `SDL_DIALOG`、引入 shim 与插件目标后，可能出现新的 `/W4 /WX` 告警 | 第三方树继续用 `SYSTEM PRIVATE` include（照 X `cmake/Cubism.cmake:99-103` 的纪律）；新文件自身零告警 |
| **工作区并发改动** | 同一工作区有另一项"垂直同步开关"改动，涉及 `config_*`/`frame_clock`/`preferences_*`/10 个 locale | 本任务**先做** CI/CMake/打包/新文件；locale 与 `preferences_*` 的改动放最后；只 `git add` 自己改的文件；绝不回退他人改动 |
| **CORE 替换时的悬空句柄** | 用户中途替换 Core → 旧的 `csm*` 函数指针可能仍被正在使用的模型对象引用 | 卸载顺序：先销毁模型实例与 GPU 资源，再 `dlclose`；旧句柄延迟释放；导入期间拒绝并发渲染 |
| **包体检查的假绿** | 断言只按文件名扫，改名夹带即可绕过 | 双扫：文件名 + 内容（magic + `csmReviveMocInPlace` 字符串）；脚本自带反例自检 |
| **降级路径静默失败** | 缺 Core 只写日志，用户以为功能坏了 | 一次性 toast + 插件页常驻入口 + 日志分级；两种缺失（缺插件 / 缺 Core）文案分离 |
| **合规回退** | 后续有人重新把 Core 链进宿主 | 构建期 `FATAL_ERROR` 守卫 + 产物级导入表断言 + CI 包体断言，三层都进版本控制 |

## P0.7 本轮执行边界（如实记录）

* 只编 `build-vulkan`；不动 `build-cubism`/`build-cubism-novk`；不发 release、不建 tag；
* 不碰 `%LOCALAPPDATA%\BongoCat`；测试一律 `--storage-root=<副本>`；
* 只按自己 `Start-Process` 的 PID 杀进程，不碰用户正在跑的实例（PID 50532）；
* 每阶段一个中文 conventional commit，推送走 `git -c http.proxy=http://127.0.0.1:7897`。

## P0.8 施工中的两项口径修正（追加，不覆盖上文）

> 这两条是施工期间由用户明确定下的口径变更。**上文 §9.1/§9.2/§9.6/§P0.3/§P0.5 的对应写法
> 已按本节作废**，保留原文只为记录决策过程。

### 修正 1：取消阶段 3（不兼容 X 的插件），目录与命名统一为 `cores`

**决定**：只把 Live2D SDK 拆成我们自己的组件，**不再兼容 `TianMengLucky/BongoCat-X` 的插件**
（连"描述符发现 + 精确报错"也不做），因此：

* §P0.3 的**阶段 3 整体取消**；§P0.5 的选项 D 也不再实施（该节结论仍然有效，只是不再落地）；
* §9.2 的 `<存储根>/plugins/`、`<exe>/plugins/`、`$BONGO_CAT_PLUGIN_DIRECTORY` 与
  §9.1 的 `<存储根>/live2d-core/` **全部改名/合并**为 `cores`；
* "插件"一词在本项目里不再使用（**没有第三方插件生态**，继续叫插件会误导用户以为可以装别人的东西）。

**修正后的目录约定（唯一口径）**：

| 归属 | 路径 | 说明 |
| --- | --- | --- |
| **我们编译的渲染组件** | `<exe 目录>/cores/bongo_live2d.dll`（Linux `libbongo_live2d.so`、macOS `libbongo_live2d.dylib`） | 内含 Cubism **Framework**（现场编译）与我们的 GL/VK 桥接，**不含 Core**；随发行包分发。放 exe 同级而不是用户数据目录，是因为它属于程序自身、版本必须与 exe 同步 —— 放数据目录会出现"换了 exe 但组件还是旧版"的错配 |
| 组件的回退/覆盖位置 | `$BONGO_CAT_CORES_DIRECTORY` → `<存储根>/cores/` | 排障、CI、绿色版把组件放别处 |
| **用户自备的 Cubism Core**（专有库） | `<存储根>/cores/<平台>/<架构>/Live2DCubismCore.dll`（`libLive2DCubismCore.so/.dylib`） | 便携模式下即 `BongoCatData\cores\...`，整体拷走仍可用；导入过程在 `<存储根>/cores/.staging/` 里完成，元数据是 `<存储根>/cores/core.json` |
| Core 的覆盖/投放点 | `$BONGO_CAT_LIVE2D_CORE` → `<存储根>/cores/...` → `<exe 目录>/cores/...` | 每一级都要过"三符号探测"才算命中 |

两个 `cores` 目录的**归属**必须分清：exe 同级那份是我们发的（随版本升级、可被程序替换），
存储根那份是用户的闭源资产（误删代价高、升级节奏与我们无关）。这也正是"发行包内无 Core"
与"用户随时换 Core"能同时成立的原因。

**连带改动**：CMake 选项 `BONGO_CAT_REQUIRE_RENDERER_PLUGIN` → `BONGO_CAT_REQUIRE_LIVE2D_COMPONENT`；
合规审计函数改为断言 `cores/bongo_live2d.*` 存在；包体检查参数 `-ExpectLive2DPlugin` →
`-ExpectLive2DComponent`；文案键 `native.live2dPluginMissing` → `native.live2dComponentMissing`；
10 语言文案、日志、CI、打包脚本里的 `plugins` 字样一并改为 `cores`。

**由此产生的一条事实（供以后回答"为什么不支持 X 的插件"）**：我们自己的组件 ABI
（`bongo_cat_renderer_module_query` + 版本化描述符 + 宿主服务表）与 BongoCat-X 的
`bongo_cat_model_plugin_query` 那套**互不兼容** —— 两者的宿主服务表内容、跨界类型与图形
契约都不同；X 编译出的 `bongo_live2d.dll` 放进我们的 `cores` 目录只会被判为"缺导出符号"
并写进日志，不会被加载。

### 修正 2："包内无 Core"的第二遍扫描改为按**动态依赖表**，不按字节符号名

§9.6 与 §P0.3 原本要求"按内容再扫一遍（magic + `csmReviveMocInPlace` 字符串）以防改名夹带"。
施工时发现这条会**误报我们自己的合法组件**：组件里的 shim 必须把 `csmReviveMocInPlace`
这类符号名当字符串交给宿主的 `core_symbol`，所以字节里必然出现这些名字。

**改为**：

1. 按文件名扫（`^lib?Live2DCubismCore([._-].*)?\.(dll|so|dylib|lib|a)$`）—— 命中即失败；
2. 按**动态依赖表**扫（Linux `readelf -d`、macOS `otool -L`）—— 依赖里出现 `Live2DCubismCore`
   即失败。这既不会误报，又正好对应"是否链接/加载了专有库"这个有法律含义的事实；
3. 静态链接这一形态（不出现在依赖表里）由**构建期**的链接级守卫
   `cmake/CubismCompliance.cmake` 的 `bongo_cat_assert_no_cubism_core_link()` 拦 —— 它递归检查
   目标链接图，命中即 `FATAL_ERROR`（迁移期由 `BONGO_CAT_CORE_LINK_TRANSITION` 降级为告警，
   阶段 2 删除该开关后自动变成硬失败）。

分工原则：**构建图归 CMake 管，产物归 CI 管** —— 两处都不重复实现对方的判定。
