# 构建环境恢复记录（C 盘重装之后）

C 盘被格式化后，装在 C 盘的东西全部丢失，装在其它盘的东西还在。这份记录把恢复
过程写下来，以后再遇到同样情况照着做即可。

## 盘点

| 组件 | 位置 | C 盘重置后 |
|---|---|---|
| Visual Studio（MSBuild 18.9.1 / MSVC 14.51.36231） | `D:\Visual Studio 2022` | 还在 ✓ |
| CMake / Ninja（VS 自带） | `D:\Visual Studio 2022\Common7\IDE\CommonExtensions\Microsoft\CMake\...` | 还在 ✓ |
| Vulkan SDK 1.4.357.0 | `G:\VulkanSDK\1.4.357.0` | 还在 ✓ |
| Windows SDK 10.0.28000.0 | 原本在 `C:\Program Files (x86)\Windows Kits\10` | **没了** |
| Git | `C:\Program Files\Git` | 重装过 ✓ |
| 机器级 PATH / 环境变量 | — | 被重置，git / cmake 都不在 PATH 里 |

## 恢复步骤

1. 安装 Windows SDK 10.0.28000.2705（与项目里固定的
   `WindowsTargetPlatformVersion=10.0.28000.0` 对齐），装到项目盘：

   ```
   winsdksetup.exe /quiet /features + /installpath "G:\Windows Kits\10" \
       /norestart /ceip off /log <logfile>
   ```

   安装包会自己提升 UAC（本机 `ConsentPromptBehaviorAdmin=0`，不会弹窗）。
   装完注册表 `HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots\KitsRoot10`
   指向 `G:\Windows Kits\10\`，MSBuild 就能找到 SDK 了（连 cdb.exe 一起回来了，
   崩溃定位要用）。

2. 恢复环境变量（机器级）：

   - `VK_SDK_PATH` = `VULKAN_SDK` = `G:\VulkanSDK\1.4.357.0`；
   - PATH 追加 `C:\Program Files\Git\cmd`、VS 自带的 CMake/Ninja 目录、
     `G:\VulkanSDK\1.4.357.0\Bin`。

   Cubism SDK 的着色器规则直接引用 `$ENV{VK_SDK_PATH}/Bin/glslc.exe`
   （见 `cmake/Cubism.cmake`），少了这个变量，Vulkan 配置能过、编译到着色器
   那一步会报 “系统找不到指定的路径” + MSB8066。

3. 执行策略：`Set-ExecutionPolicy -Scope CurrentUser RemoteSigned`。
   重置后是 Restricted，`build-vulkan/.test/*.ps1` 这些验证脚本全都跑不起来。

## VS 实例未注册

C 盘重置把 Visual Studio Installer 的注册信息一起清掉了，CMake 的 VS 生成器会报：

```
Generator Visual Studio 18 2026 could not find specified instance of Visual Studio:
  D:/Visual Studio 2022  (the instance is not known to the Visual Studio Installer,
  and no 'version=' field was given)
```

配置时显式给出实例版本即可（CMake 会跳过 Installer 注册检查）：

```
cmake -S . -B build-cubism -G "Visual Studio 18 2026" \
  -DCMAKE_GENERATOR_INSTANCE="D:/Visual Studio 2022,version=18.9.12105.275"
```

版本号取自 `D:\Visual Studio 2022\Common7\IDE\devenv.exe` 的文件版本（18.9.12105.275）。
同样的值已经写进机器级环境变量 `CMAKE_GENERATOR_INSTANCE`。

## 编译三个构建树

```
set VK_SDK_PATH=G:\VulkanSDK\1.4.357.0
set VULKAN_SDK=G:\VulkanSDK\1.4.357.0
MSBuild.exe build-cubism\bongo_cat.vcxproj /p:Configuration=Release /m:6 /nodeReuse:false /v:m
```

`build-vulkan`（Cubism + Vulkan）、`build-cubism-novk`（Cubism，Vulkan 关）同理。
三者都已用恢复后的环境完整编过一遍，`Windows import audit passed`。
