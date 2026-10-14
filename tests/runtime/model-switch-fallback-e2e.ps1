#Requires -Version 7
<#
.SYNOPSIS
  换桌宠"失败回退"的端到端验证。

.DESCRIPTION
  验证 model_switch.c 的失败回退在**真实主循环**里成立: 切到坏模型时停留在原模型、
  弹出 i18n 失败提示、不崩溃、主循环继续出帧; 连点多个模型时只应用最后一次; 切到"当前
  已经加载的模型"时不重新加载也不崩; 顺序轮换遇到坏模型不会卡死; 副宠进程由主进程拉起
  并收到控制文件。

  为什么要单独一个脚本 (而不是一条 ctest): 这里每一条结论都只能靠"真的起一个进程、真的
  开窗、真的出帧"量出来 —— 帧间隔、窗口是否还画得出来、设置页是否还响应、进程有没有崩。
  ctest 里目前**没有**覆盖换模型队列的用例 (tests/ 下只有 update / import / live2d 等
  几组), 所以这些不变量暂时只有本脚本在守; 代价是它需要一块真实桌面, 不能进 CI 的无人值守
  流水线。想补确定性覆盖的话, 最该补的是"空影子不许进 commit_model_switch"这一条 ——
  它是下面那个崩溃的直接根因。

  这个脚本第一次跑就抓到一个真崩溃 (same-model / rapid 两个场景): 请求切换到的模型正好
  就是当前已加载的那个时 (设置页点自己已选中的卡片、右键菜单选当前模型、连点若干次后绕回
  原模型), model_switch_step 会用空影子走完整提交, 而提交会解引用 shadow->behaviors,
  于是主循环直接 0xC0000005。修复见 model_files.c 的 bongo_cat_app_recommit_loaded_model。
  这两个场景现在就是那条修复的回归哨兵。

  隔离手段 (绝不碰用户真实配置/模型):
    * 每个场景一个全新的 --storage-root 副本 (内置模型由程序自己装进去);
    * BONGO_CAT_ELEVATION_TRIED=1 不弹 UAC, BONGO_CAT_ALLOW_TEST_INSTANCES=1 允许并存;
    * --nearby-root 指向一个空目录, 免得扫描工作目录把无关模型带进目录表;
    * 用户那份 settings.json 只当结构模板, 多宠/隐藏/移除/自定义缓存路径一律显式中性化;
    * 只用 Start-Process 返回的 PID 收尾 (Stop-Process -Id), 绝不按名字杀进程。

  已知的既有问题 (与本脚本无关, 断言里按"运行中没崩"处理):
    Vulkan 后端在进程退出后必崩 0xC0000005 (应用自己的 shutdown 已经完整走完)。
    用未改动的 HEAD 二进制 + `--renderer=vulkan` 不带任何换模型参数跑一次即可复现,
    所以判据用日志里的 "Shutdown complete: exit_code=0" 而不是进程退出码。

.PARAMETER Exe
  被测可执行文件。建议指向构建产物的副本 —— 不要打断用户正在用的那个实例。

.PARAMETER Backend
  vulkan 或 opengl。

.PARAMETER WorkRoot
  工作目录 (存放 storage 副本、日志、截图)。默认放在 $env:TEMP 下。

.PARAMETER Scenario
  要跑的场景, 逗号分隔; all 表示全部。可选:
    bad-json / bad-fields / bad-moc3 / bad-moc3-missing / bad-texture
        —— 逐个破坏内置 keyboard 模型的不同部件, 验证失败回退;
    same-model  —— 切到当前已加载的模型 (历史崩溃点);
    rapid       —— 连点 5 个 (含坏模型), 只应用最后一次;
    rotation    —— 顺序轮换遇到坏模型;
    secondary / secondary-bad —— 副宠进程 (正常 / 副模型是坏的)。

.PARAMETER ReferenceSettings
  可选: 一份 settings.json 作为基准 (顺序轮换场景会在它基础上打开 sequentialModel)。

.EXAMPLE
  pwsh -File tests/runtime/model-switch-fallback-e2e.ps1 `
    -Exe $env:TEMP\bcat\BongoCatTurbo.exe -Backend vulkan -Scenario bad-json,rapid
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [ValidateSet('vulkan', 'opengl')][string]$Backend = 'vulkan',
    [string]$WorkRoot = '',
    # 刻意不用 ValidateSet: 那样 "-Scenario bad-json,rapid" 会被当成一个整体而报错
    # (pwsh -File 不会把逗号拆成数组), 而 .EXAMPLE 里写的正是这个用法。这里改为自己在
    # 下面拆分 + 校验, 于是 -File 与 & 两种调用方式下写法一致。
    [string[]]$Scenario = @('all'),
    [string]$ReferenceSettings = '',
    # 关键场景的帧间隔上界 (毫秒)。8ms 帧预算 + 单步最长耗时决定上界, 实测几百毫秒;
    # 留到 2.5s 是为了容忍软件 OpenGL / CI 机器的抖动, 同时仍然能抓住"整段不出帧"。
    [int]$MaxFrameGapMs = 2500,
    [int]$OriginalModelExitMs = 9000,
    [int]$RotationExitMs = 26000,
    [int]$SecondaryExitMs = 22000
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# 场景清单 (顺序即"全跑"时的执行顺序)。'-Scenario a,b' 在 -File 调用下整个串是一个元素,
# 所以统一再按逗号拆一次, 让两种写法等价; 写错的场景名直接报错, 不静默跳过。
$script:AllScenarios = @('bad-json', 'bad-fields', 'bad-moc3', 'bad-moc3-missing',
    'bad-texture', 'same-model', 'rapid', 'rotation', 'secondary', 'secondary-bad')
$Scenario = @($Scenario | ForEach-Object { $_ -split ',' } |
    Where-Object { $_ -ne '' } | ForEach-Object { $_.Trim() })
if ($Scenario -contains 'all') { $Scenario = $script:AllScenarios }
$unknown = @($Scenario | Where-Object { $script:AllScenarios -notcontains $_ })
if ($unknown.Count) {
    throw ("unknown scenario(s): {0}; valid: {1}" -f ($unknown -join ', '),
        ($script:AllScenarios -join ', '))
}
if (-not $Scenario.Count) { throw 'no scenario selected' }

$script:Exe = (Resolve-Path -LiteralPath $Exe).Path
if (-not $WorkRoot) {
    $WorkRoot = Join-Path $env:TEMP ('bongocat-switch-e2e-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
}
$script:Work = [IO.Path]::GetFullPath($WorkRoot)
$script:EmptyNearby = Join-Path $script:Work 'empty-nearby'
$script:Results = [System.Collections.Generic.List[object]]::new()
$script:Logs = [System.Collections.Generic.List[string]]::new()
$script:Evidence = [System.Collections.Generic.List[string]]::new()

New-Item -ItemType Directory -Force $script:Work, $script:EmptyNearby | Out-Null

# ---------------------------------------------------------------- 断言与记录

function Add-Result {
    param([string]$Scenario, [string]$Check, [bool]$Pass, [string]$Detail)
    $script:Results.Add([pscustomobject]@{
            Scenario = $Scenario; Check = $Check; Pass = $Pass; Detail = $Detail
        })
    $mark = if ($Pass) { 'PASS' } else { 'FAIL' }
    Write-Host ("  [{0}] {1}: {2}" -f $mark, $Check, $Detail)
}

function Assert-Contains {
    param([string]$Scenario, [string]$Name, [string[]]$Lines, [string]$Pattern)
    $hit = @($Lines | Select-String -Pattern $Pattern)
    $detail = if ($hit.Count) { $hit[0].Line.Trim() } else { "no line matching /$Pattern/" }
    Add-Result $Scenario $Name ($hit.Count -gt 0) $detail
}

function Assert-True {
    param([string]$Scenario, [string]$Name, [bool]$Condition, [string]$Detail)
    Add-Result $Scenario $Name $Condition $Detail
}

# ---------------------------------------------------------------- 存储副本

function New-Storage {
    param([string]$Name)
    $path = Join-Path $script:Work $Name
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force }
    foreach ($tree in 'config', 'data', 'models', 'cache', 'state', 'logs') {
        New-Item -ItemType Directory -Force (Join-Path $path $tree) | Out-Null
    }
    return $path
}

# 基准配置: 优先用调用方给的, 否则用平台默认的那份 (只读复制, 不改用户文件)。
# 统一改掉两项: 副宠/自测不需要提权, 顺序轮换默认关闭 (只有 rotation 场景自己打开)。
function Write-Settings {
    param([string]$Storage, [hashtable]$Patch = @{})
    $source = $ReferenceSettings
    if (-not $source) { $source = Join-Path $env:LOCALAPPDATA 'BongoCat\config\settings.json' }
    $settings = $null
    if ($source -and (Test-Path -LiteralPath $source)) {
        $settings = Get-Content -LiteralPath $source -Raw | ConvertFrom-Json -AsHashtable
    }
    if (-not $settings) {
        $settings = @{ format = 'bongocat/settings'; schemaVersion = 1 } + @{}
    }
    if (-not $settings.ContainsKey('window')) { $settings['window'] = @{} }
    if (-not $settings.ContainsKey('application')) { $settings['application'] = @{} }
    if (-not $settings.ContainsKey('rendering')) { $settings['rendering'] = @{} }
    $settings['application']['runAsAdmin'] = $false
    $settings['window']['sequentialModel'] = $false
    $settings['window']['sequentialModelIntervalSeconds'] = 600.0
    # 用户那份配置只当"文档结构模板"用, 与隔离有关的字段一律显式中性化。不这样做的话,
    # 本机开着多宠、隐藏过某个内置模型、或配了自定义 cachePath 时, 场景的前提 (三个内置
    # 模型都在、只跑一个桌宠、资源解到副本里) 会静默不成立, 断言就会以别的原因失败。
    $settings['cachePath'] = ''
    $settings['hiddenModels'] = @()
    $settings['removedModels'] = @()
    $settings['modelOverrides'] = @()
    $settings['randomDisabled'] = @()
    $settings['rendering']['multiplePets'] = $false
    foreach ($key in $Patch.Keys) {
        $parts = $key.Split('.')
        $settings[$parts[0]][$parts[1]] = $Patch[$key]
    }
    $target = Join-Path $Storage 'config\settings.json'
    $settings | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $target -Encoding utf8
    return $target
}

# ------------------------------------------------------------------ 启动进程
# 只用自己 Start-Process 得到的 PID 收尾; 超时才 Stop-Process -Id, 绝不按名字杀。
function Start-Pet {
    param([string]$Storage, [string]$Tag, [string[]]$Extra, [string]$InstanceId)
    $env:BONGO_CAT_ELEVATION_TRIED = '1'
    $env:BONGO_CAT_ALLOW_TEST_INSTANCES = '1'
    $env:BONGO_CAT_TEST_INSTANCE_ID = $InstanceId
    $arguments = @("--storage-root=$Storage", "--nearby-root=$script:EmptyNearby",
        "--renderer=$Backend") + $Extra
    $out = Join-Path $script:Work "$Tag.out.txt"
    $err = Join-Path $script:Work "$Tag.err.txt"
    $process = Start-Process -FilePath $script:Exe -ArgumentList $arguments -PassThru `
        -WorkingDirectory $script:Exe.Substring(0, $script:Exe.LastIndexOf('\')) `
        -RedirectStandardOutput $out -RedirectStandardError $err
    Write-Host ("  [start] {0} pid={1}" -f $Tag, $process.Id)
    return [pscustomobject]@{
        Process = $process; Tag = $Tag; Storage = $Storage; Out = $out; Err = $err
    }
}

function Stop-Pet {
    param($Run, [int]$TimeoutSeconds = 60)
    if (-not $Run.Process.WaitForExit($TimeoutSeconds * 1000)) {
        Write-Host ("  [stop] {0} 未在 {1}s 内退出 -> Stop-Process -Id {2}" -f `
                $Run.Tag, $TimeoutSeconds, $Run.Process.Id)
        Stop-Process -Id $Run.Process.Id -Force -ErrorAction SilentlyContinue
        $Run.Process.WaitForExit(15000) | Out-Null
    }
    return $Run.Process.ExitCode
}

function Get-AppLog {
    param([string]$Storage)
    $path = Join-Path $Storage 'logs\BongoCat.log'
    if (-not (Test-Path -LiteralPath $path)) { return @() }
    return @(Get-Content -LiteralPath $path)
}

function Get-FrameStats {
    param([string]$Storage)
    $path = Join-Path $Storage 'state\frame-interval.txt'
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    $text = (Get-Content -LiteralPath $path -Raw).Trim()
    $stats = @{}
    foreach ($token in ($text -split '\s+')) {
        $pair = $token -split '=', 2
        if ($pair.Count -eq 2) { $stats[$pair[0]] = $pair[1] }
    }
    return [pscustomobject]@{
        Raw = $text; Frames = [int]$stats['frames']; MaxMs = [double]$stats['max_ms']
        P99Ms = [double]$stats['p99_ms']; MedianMs = [double]$stats['median_ms']
        Path = $path
    }
}

# ------------------------------------------------------------ 窗口截图与消息
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class E2EWindow {
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] static extern IntPtr GetWindowDC(IntPtr h);
    [DllImport("user32.dll")] static extern int ReleaseDC(IntPtr h, IntPtr dc);
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("gdi32.dll")] static extern IntPtr CreateCompatibleDC(IntPtr dc);
    [DllImport("gdi32.dll")] static extern IntPtr CreateCompatibleBitmap(IntPtr dc, int w, int h);
    [DllImport("gdi32.dll")] static extern IntPtr SelectObject(IntPtr dc, IntPtr obj);
    [DllImport("gdi32.dll")] static extern bool DeleteObject(IntPtr obj);
    [DllImport("gdi32.dll")] static extern bool DeleteDC(IntPtr dc);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    delegate bool EnumProc(IntPtr h, IntPtr p);
    public static List<IntPtr> ForPid(uint target, string prefix) {
        var found = new List<IntPtr>();
        EnumWindows((h, p) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid != target) return true;
            var sb = new StringBuilder(512); GetWindowTextW(h, sb, sb.Capacity);
            if (sb.ToString().StartsWith(prefix)) found.Add(h);
            return true;
        }, IntPtr.Zero);
        return found;
    }
    public static IntPtr CaptureBitmap(IntPtr hwnd, out int width, out int height) {
        width = 0; height = 0;
        RECT rect; if (!GetWindowRect(hwnd, out rect)) return IntPtr.Zero;
        int w = rect.Right - rect.Left, h = rect.Bottom - rect.Top;
        if (w < 8 || h < 8) return IntPtr.Zero;
        IntPtr dc = GetWindowDC(hwnd);
        IntPtr mem = CreateCompatibleDC(dc);
        IntPtr bmp = CreateCompatibleBitmap(dc, w, h);
        IntPtr old = SelectObject(mem, bmp);
        bool ok = PrintWindow(hwnd, mem, 0x00000002 /* PW_RENDERFULLCONTENT */);
        SelectObject(mem, old); DeleteDC(mem); ReleaseDC(hwnd, dc);
        if (!ok) { DeleteObject(bmp); return IntPtr.Zero; }
        width = w; height = h;
        return bmp;
    }
    public static void FreeBitmap(IntPtr bmp) { if (bmp != IntPtr.Zero) DeleteObject(bmp); }
}
'@

# 位图从 GDI 拿到之后交给 System.Drawing 编码成 PNG; 这里刻意不在 C# 里引用
# System.Drawing (它在本机是转发类型, Add-Type 不带引用会编译失败)。
function Save-Shot {
    param([IntPtr]$Hwnd, [string]$Path)
    if ($Hwnd -eq [IntPtr]::Zero -or -not [E2EWindow]::IsWindow($Hwnd)) { return $false }
    $width = 0; $height = 0
    $handle = [E2EWindow]::CaptureBitmap($Hwnd, [ref]$width, [ref]$height)
    if ($handle -eq [IntPtr]::Zero) { return $false }
    try {
        $bitmap = [System.Drawing.Image]::FromHbitmap($handle)
        $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
        $bitmap.Dispose()
        return $true
    } finally {
        [E2EWindow]::FreeBitmap($handle)
    }
}

function Get-FileHashString {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return '' }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

function Wait-Window {
    param([int]$Pid_, [string]$Prefix, [int]$TimeoutSeconds = 25)
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $found = [E2EWindow]::ForPid([uint32]$Pid_, $Prefix)
        foreach ($hwnd in $found) { if ([E2EWindow]::IsWindowVisible($hwnd)) { return $hwnd } }
        Start-Sleep -Milliseconds 200
    }
    return [IntPtr]::Zero
}

function Get-PreferencesHandle {
    param([string]$Storage)
    $path = Join-Path $Storage 'state\preferences-window.txt'
    if (-not (Test-Path -LiteralPath $path)) { return [IntPtr]::Zero }
    foreach ($token in ((Get-Content -LiteralPath $path -Raw) -split '\s+')) {
        if ($token.StartsWith('handle=')) {
            $value = [uint64]$token.Substring(7)
            if ($value) { return [IntPtr][int64]$value }
        }
    }
    return [IntPtr]::Zero
}

# ------------------------------------------------------------------ 破坏模型
function Set-Corrupt {
    param([string]$Storage, [string]$Model, [string]$Kind)
    $dir = Join-Path $Storage "models\$Model"
    if (-not (Test-Path -LiteralPath $dir)) { throw "model not installed: $dir" }
    switch ($Kind) {
        'bad-json' {
            # 语法损坏: 截断在半个键上, 宽松解析器也救不回来 (缺值)。
            Set-Content -LiteralPath (Join-Path $dir 'cat.model3.json') `
                -Value '{"Version":3,"FileReferences":{"Moc":' -NoNewline -Encoding ascii
        }
        'bad-fields' {
            # 字段损坏: 合法 JSON 但没有 Version/FileReferences, 过不了预校验。
            Set-Content -LiteralPath (Join-Path $dir 'cat.model3.json') `
                -Value '{}' -NoNewline -Encoding ascii
        }
        'bad-moc3' {
            # 截断: 只留前 512 字节 (moc3 头声称的长度与实际不符)。
            $moc = Join-Path $dir 'demomodel2.moc3'
            $bytes = [IO.File]::ReadAllBytes($moc)
            [IO.File]::WriteAllBytes($moc, $bytes[0..511])
        }
        'bad-moc3-missing' { Remove-Item -LiteralPath (Join-Path $dir 'demomodel2.moc3') -Force }
        'bad-texture' { Remove-Item -LiteralPath (Join-Path $dir 'demomodel2.1024\texture_01.png') -Force }
        default { throw "unknown corruption kind: $Kind" }
    }
    Write-Host "  [corrupt] $Model <- $Kind"
}

# 内置模型由程序自己装进 models_root; 只有"目录已存在"它才会跳过复制, 所以先空跑一次把
# 三个内置模型落地, 之后对副本的破坏才能稳定生效 (否则下一次启动又会被重新覆盖)。
function Install-Builtins {
    param([string]$Storage, [string]$Tag)
    $run = Start-Pet -Storage $Storage -Tag $Tag -Extra @('--ci-smoke', '--ci-exit-ms=1500') `
        -InstanceId ("e2ewarm" + [guid]::NewGuid().ToString('N').Substring(0, 6))
    Stop-Pet -Run $run -TimeoutSeconds 60 | Out-Null
    foreach ($model in 'standard', 'keyboard', 'gamepad') {
        $dir = Join-Path $Storage "models\$model"
        if (-not (Test-Path -LiteralPath (Join-Path $dir 'cat.model3.json'))) {
            throw "built-in model was not installed: $dir"
        }
    }
}

# ------------------------------------------------------------------ 场景实现

# 场景 1: 坏模型回退。期望 = 停留原模型 + 失败提示 + 不崩 + 继续出帧 + 画面仍在动。
function Invoke-BadModel {
    param([string]$Kind, [int]$Index)
    $name = $Kind
    Write-Host "== 场景 $name ($Backend) =="
    $storage = New-Storage "$name-$Backend"
    Write-Settings -Storage $storage | Out-Null
    Install-Builtins -Storage $storage -Tag "$name-warm"
    Set-Corrupt -Storage $storage -Model 'keyboard' -Kind $Kind
    $run = Start-Pet -Storage $storage -Tag "$name-$Backend" -InstanceId ("e2e$Kind" + $Index) -Extra @(
        '--ci-model=standard', '--ci-model-switch=keyboard', '--ci-frame-series',
        '--ci-preferences', "--ci-exit-ms=$OriginalModelExitMs")
    $pet = Wait-Window -Pid_ $run.Process.Id -Prefix 'BongoCat Turbo'
    $shots = @{}
    $shots['before'] = Join-Path $script:Work "$name-before.png"
    Save-Shot -Hwnd $pet -Path $shots['before'] | Out-Null
    Start-Sleep -Milliseconds 2200   # 让 2.5s 之后的切换真的发生
    $shots['after-1'] = Join-Path $script:Work "$name-after-1.png"
    Save-Shot -Hwnd $pet -Path $shots['after-1'] | Out-Null
    Start-Sleep -Milliseconds 700
    $shots['after-2'] = Join-Path $script:Work "$name-after-2.png"
    Save-Shot -Hwnd $pet -Path $shots['after-2'] | Out-Null
    $prefs = Get-PreferencesHandle -Storage $storage
    $shots['notice'] = Join-Path $script:Work "$name-notice.png"
    Save-Shot -Hwnd $prefs -Path $shots['notice'] | Out-Null
    $exit = Stop-Pet -Run $run -TimeoutSeconds ([int]($OriginalModelExitMs / 1000) + 40)
    $lines = Get-AppLog -Storage $storage
    $frames = Get-FrameStats -Storage $storage

    Assert-Contains $name '失败并回退到原模型' $lines 'Model switch failed and rolled back: previous=standard next=keyboard'
    Assert-Contains $name '设置页弹出失败提示 (i18n native.modelSwitchFailed 路径)' $lines 'Preferences model switch failed: id=keyboard'
    Assert-Contains $name '最终仍停在原模型 standard' $lines 'Smoke model switch complete: requested=1 applied=standard'
    Assert-Contains $name '运行期间未崩溃 (应用自己的 shutdown 走完)' $lines 'Shutdown complete: exit_code=0'
    Assert-True $name '没有超时/卡死在"正在切换"' `
        (-not (@($lines | Select-String 'Model loading timed out').Count)) `
        ("timed-out lines: " + @($lines | Select-String 'Model loading timed out').Count)
    if ($frames) {
        Assert-True $name '主循环持续出帧' ($frames.Frames -ge 60) `
            ("frames={0} intervals={1}" -f $frames.Frames, $frames.Raw)
        Assert-True $name "最大帧间隔 <= $MaxFrameGapMs ms" ($frames.MaxMs -le $MaxFrameGapMs) `
            ("max_ms={0} p99_ms={1} median_ms={2}" -f $frames.MaxMs, $frames.P99Ms, $frames.MedianMs)
    } else {
        Assert-True $name '主循环持续出帧' $false 'state/frame-interval.txt missing'
    }
    $before = Get-FileHashString $shots['before']
    $after1 = Get-FileHashString $shots['after-1']
    $after2 = Get-FileHashString $shots['after-2']
    Assert-True $name '失败回退后原模型仍在渲染 (前后都截到了宠物窗口)' `
        ($before -and $after1 -and $after2) "shots: $($shots['before']) $($shots['after-1'])"
    Assert-True $name '失败回退后模型仍在动 (两次截图不同)' ($after1 -ne $after2) `
        "sha256 after-1=$($after1.Substring(0,12)) after-2=$($after2.Substring(0,12))"
    foreach ($key in $shots.Keys) { $script:Evidence.Add($shots[$key]) }
    $script:Evidence.Add((Join-Path $storage 'state\frame-interval.txt'))
    $script:Logs.Add((Join-Path $storage 'logs\BongoCat.log'))
    Write-Host ("  frames={0} max_ms={1} exit={2}" -f $frames.Frames, $frames.MaxMs, $exit)
}

# 场景 2: 切到"当前已经加载的模型"。这正是用户在设置页点自己已经选中的那张卡片、或在
# 右键菜单里选当前模型时会走的路, 也是连点若干次后绕回原模型的那条路。
#
# 为什么要单独一个场景: 修复前它**必然崩溃** (0xC0000005) —— model_switch_step 命中
# "已经是它"的分支后, 用一个从未准备 (或被 job_clear 释放成空) 的影子去走完整提交, 而完整
# 提交会无条件解引用 shadow->behaviors。崩溃发生在主循环里, 于是日志停在"请求已登记"那一行,
# frame-interval.txt 根本不会落盘 —— 所以这个场景同时也验证"不崩 + 继续出帧"。
function Invoke-SameModel {
    Write-Host "== 场景 same-model ($Backend) =="
    $storage = New-Storage "same-model-$Backend"
    Write-Settings -Storage $storage | Out-Null
    Install-Builtins -Storage $storage -Tag 'same-model-warm'
    $run = Start-Pet -Storage $storage -Tag "same-model-$Backend" -InstanceId 'e2esamemodel' -Extra @(
        '--ci-model=standard', '--ci-model-switch=standard', '--ci-frame-series',
        "--ci-exit-ms=$OriginalModelExitMs")
    $pet = Wait-Window -Pid_ $run.Process.Id -Prefix 'BongoCat Turbo'
    $shots = @{}
    $shots['before'] = Join-Path $script:Work 'same-model-before.png'
    Save-Shot -Hwnd $pet -Path $shots['before'] | Out-Null
    Start-Sleep -Milliseconds 3000   # 让 2.5s 之后那次"切到自己"真的发生
    $shots['after'] = Join-Path $script:Work 'same-model-after.png'
    Save-Shot -Hwnd $pet -Path $shots['after'] | Out-Null
    $exit = Stop-Pet -Run $run -TimeoutSeconds ([int]($OriginalModelExitMs / 1000) + 40)
    $lines = Get-AppLog -Storage $storage
    $frames = Get-FrameStats -Storage $storage

    Assert-Contains 'same-model' '命中"已经是它"的短路且没有重新加载' $lines `
        'Model switch already loaded: id=standard skipped_reload=1'
    Assert-Contains 'same-model' '请求正常收尾 (没有卡在"正在切换")' $lines `
        'Smoke model switch complete: requested=1 applied=standard'
    Assert-Contains 'same-model' '运行期间未崩溃 (应用自己的 shutdown 走完)' $lines `
        'Shutdown complete: exit_code=0'
    # 这两条是"用错入口"的哨兵: 真出现就说明又有调用方拿空影子去走完整提交了。
    Assert-True 'same-model' '没有触发"空影子提交"的防御性报错' `
        (-not (@($lines | Select-String 'commit called without a prepared shadow').Count)) `
        ("guard lines: " + @($lines | Select-String 'commit called without a prepared shadow').Count)
    Assert-True 'same-model' '没有把它当成一次失败 (不该出现回退提示)' `
        (-not (@($lines | Select-String 'Model switch failed and rolled back').Count)) `
        ("failed lines: " + @($lines | Select-String 'Model switch failed and rolled back').Count)
    if ($frames) {
        Assert-True 'same-model' '主循环持续出帧' ($frames.Frames -ge 60) `
            ("frames={0}" -f $frames.Frames)
        Assert-True 'same-model' "最大帧间隔 <= $MaxFrameGapMs ms" ($frames.MaxMs -le $MaxFrameGapMs) `
            ("max_ms={0} p99_ms={1} median_ms={2}" -f $frames.MaxMs, $frames.P99Ms, $frames.MedianMs)
    } else {
        Assert-True 'same-model' '主循环持续出帧' $false 'state/frame-interval.txt missing'
    }
    $before = Get-FileHashString $shots['before']
    $after = Get-FileHashString $shots['after']
    Assert-True 'same-model' '模型仍在正常渲染 (前后都截到了宠物窗口)' ($before -and $after) `
        "shots: $($shots['before']) $($shots['after'])"
    $script:Evidence.Add($shots['before']); $script:Evidence.Add($shots['after'])
    $script:Evidence.Add((Join-Path $storage 'state\frame-interval.txt'))
    $script:Logs.Add((Join-Path $storage 'logs\BongoCat.log'))
    if ($frames) { Write-Host ("  frames={0} max_ms={1} exit={2}" -f $frames.Frames, $frames.MaxMs, $exit) }
}

# 场景 3: 连点 5 个模型 (含坏模型), 最后一个必须生效; 坏的那次不能污染后一次。
function Invoke-Rapid {
    Write-Host "== 场景 rapid ($Backend) =="
    $storage = New-Storage "rapid-$Backend"
    Write-Settings -Storage $storage | Out-Null
    Install-Builtins -Storage $storage -Tag 'rapid-warm'
    Set-Corrupt -Storage $storage -Model 'keyboard' -Kind 'bad-json'
    $run = Start-Pet -Storage $storage -Tag "rapid-$Backend" -InstanceId 'e2erapid' -Extra @(
        '--ci-model=standard',
        '--ci-model-switch=gamepad,keyboard,standard,keyboard,gamepad',
        '--ci-frame-series', '--ci-preferences', "--ci-exit-ms=$OriginalModelExitMs")
    $pet = Wait-Window -Pid_ $run.Process.Id -Prefix 'BongoCat Turbo'
    # 设置窗口的句柄由程序自己写进 state/preferences-window.txt (见 preferences_smoke.c),
    # 比按标题猜可靠 —— 宠物窗口的标题也以 "BongoCat" 开头。
    $prefs = [IntPtr]::Zero
    $deadline = (Get-Date).AddSeconds(25)
    while ((Get-Date) -lt $deadline -and $prefs -eq [IntPtr]::Zero) {
        $prefs = Get-PreferencesHandle -Storage $storage
        if ($prefs -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 200 }
    }
    $shotA = Join-Path $script:Work 'rapid-prefs-a.png'
    Save-Shot -Hwnd $prefs -Path $shotA | Out-Null
    # 设置页响应性: 轮播期间连点结束之后再发一次滚轮 + WM_CLOSE, 窗口必须真的关掉,
    # 而宠物进程要活着继续出帧。
    $frameCsv = Join-Path $storage 'state\preferences-frames.csv'
    $csvBefore = if (Test-Path -LiteralPath $frameCsv) { (Get-Content -LiteralPath $frameCsv).Count } else { 0 }
    Start-Sleep -Milliseconds 3500
    $csvAfter = if (Test-Path -LiteralPath $frameCsv) { (Get-Content -LiteralPath $frameCsv).Count } else { 0 }
    $shotB = Join-Path $script:Work 'rapid-prefs-b.png'
    $prefsNow = Get-PreferencesHandle -Storage $storage
    Save-Shot -Hwnd $prefsNow -Path $shotB | Out-Null
    # 滚轮: 页面必须消费它 (设置窗口的事件循环没卡住)
    if ($prefsNow -ne [IntPtr]::Zero) {
        [E2EWindow]::PostMessageW($prefsNow, 0x020A, [IntPtr]0x00780000, [IntPtr]0x00C800C8) | Out-Null
    }
    Start-Sleep -Milliseconds 600
    $closed = $false
    if ($prefsNow -ne [IntPtr]::Zero) {
        [E2EWindow]::PostMessageW($prefsNow, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null  # WM_CLOSE
        $deadline = (Get-Date).AddSeconds(6)
        while ((Get-Date) -lt $deadline) {
            if (-not [E2EWindow]::IsWindow($prefsNow)) { $closed = $true; break }
            Start-Sleep -Milliseconds 150
        }
    }
    $aliveAfterClose = -not $run.Process.HasExited
    $framesBeforeExit = Get-FrameStats -Storage $storage
    $exit = Stop-Pet -Run $run -TimeoutSeconds ([int]($OriginalModelExitMs / 1000) + 40)
    $lines = Get-AppLog -Storage $storage
    $frames = Get-FrameStats -Storage $storage

    # 按 next= 分别数, 而不是只数 complete 的条数: 启动时那一次 (--ci-model=standard) 也会
    # 打一条 complete, 所以"总共几条"说明不了任何事。真正要断言的是:
    #   * next=gamepad 恰好 1 条 —— 连点 5 个里只有最后一个真正落地;
    #   * next=keyboard 0 条 —— 坏模型从未被提交 ("坏的那次没有污染后一次")。
    $completes = @($lines | Select-String 'Model switch transaction: stage=complete')
    $applied = @($completes | Where-Object { $_.Line -match 'next=gamepad' })
    $badCommits = @($completes | Where-Object { $_.Line -match 'next=keyboard' })
    $supersedes = @($lines | Select-String 'Model switch superseded')
    $skipped = @($lines | Select-String 'Model switch already loaded: id=standard')
    Assert-Contains 'rapid' '连点 5 个模型后只应用最后一次 (applied=gamepad)' $lines `
        'Smoke model switch complete: requested=5 applied=gamepad'
    Assert-True 'rapid' '只有最后一次 (gamepad) 真正提交' ($applied.Count -eq 1) `
        ("complete(next=gamepad)={0}; all completes={1}" -f $applied.Count, $completes.Count)
    Assert-True 'rapid' '坏模型 keyboard 从未提交' ($badCommits.Count -eq 0) `
        ("complete(next=keyboard)={0}" -f $badCommits.Count)
    Assert-True 'rapid' '被顶掉的请求走了 supersede 丢弃路径' ($supersedes.Count -ge 2) `
        ("superseded lines={0}" -f $supersedes.Count)
    # 连点里第 3 个请求正好是当前已加载的模型: 它必须走"已经是它"的短路。这条断言是那个
    # 空指针崩溃 (0xC0000005) 的回归哨兵 —— 修复前光标到这里整个进程就没了。
    Assert-True 'rapid' '连点里绕回原模型时不重新加载 (崩溃回归哨兵)' ($skipped.Count -ge 1) `
        ("already-loaded lines={0}" -f $skipped.Count)
    Assert-Contains 'rapid' '坏的那次没有污染最后一次' $lines 'applied=gamepad'
    Assert-Contains 'rapid' '运行期间未崩溃 (应用自己的 shutdown 走完)' $lines 'Shutdown complete: exit_code=0'
    Assert-True 'rapid' '设置页在连点期间持续出帧' ($csvAfter -gt $csvBefore) `
        ("preferences-frames.csv {0} -> {1} ({2})" -f $csvBefore, $csvAfter, $frameCsv)
    Assert-True 'rapid' '设置页能收到 WM_CLOSE 并真的关闭' $closed `
        ("hwnd={0}" -f $prefsNow)
    Assert-True 'rapid' '设置页关闭后宠物进程仍在出帧' $aliveAfterClose 'process alive after WM_CLOSE'
    if ($frames) {
        Assert-True 'rapid' '主循环持续出帧' ($frames.Frames -ge 100) ("frames={0}" -f $frames.Frames)
        Assert-True 'rapid' "最大帧间隔 <= $MaxFrameGapMs ms" ($frames.MaxMs -le $MaxFrameGapMs) `
            ("max_ms={0} p99_ms={1} median_ms={2}" -f $frames.MaxMs, $frames.P99Ms, $frames.MedianMs)
    } else {
        Assert-True 'rapid' '主循环持续出帧' $false 'state/frame-interval.txt missing'
    }
    $script:Evidence.Add($shotA); $script:Evidence.Add($shotB)
    $script:Evidence.Add((Join-Path $storage 'state\frame-interval.txt'))
    $script:Logs.Add((Join-Path $storage 'logs\BongoCat.log'))
    Write-Host ("  completes={0} supersedes={1} exit={2}" -f $completes.Count, $supersedes.Count, $exit)
}

# 场景 3: 顺序轮换遇到坏模型。要求: 不卡在"正在切换"; 能提示; 轮换不永远停在坏模型上。
function Invoke-Rotation {
    Write-Host "== 场景 rotation ($Backend) =="
    $storage = New-Storage "rotation-$Backend"
    Write-Settings -Storage $storage -Patch @{
        'window.sequentialModel' = $true
        'window.sequentialModelIntervalSeconds' = 4.0
    } | Out-Null
    Install-Builtins -Storage $storage -Tag 'rotation-warm'
    Set-Corrupt -Storage $storage -Model 'keyboard' -Kind 'bad-json'
    $run = Start-Pet -Storage $storage -Tag "rotation-$Backend" -InstanceId 'e2erotate' -Extra @(
        '--ci-model=standard', '--ci-frame-series', '--ci-preferences',
        "--ci-exit-ms=$RotationExitMs")
    $pet = Wait-Window -Pid_ $run.Process.Id -Prefix 'BongoCat Turbo'
    Start-Sleep -Seconds ([int]($RotationExitMs / 1000) - 3)
    $shot = Join-Path $script:Work 'rotation-final.png'
    Save-Shot -Hwnd $pet -Path $shot | Out-Null
    $exit = Stop-Pet -Run $run -TimeoutSeconds ([int]($RotationExitMs / 1000) + 40)
    $lines = Get-AppLog -Storage $storage
    $frames = Get-FrameStats -Storage $storage
    $completes = @($lines | Select-String 'Model switch transaction: stage=complete')
    $rotations = @($lines | Select-String 'Model switch (failed and rolled back|transaction: stage=complete)')
    $reached = @($completes | ForEach-Object {
            if ($_.Line -match 'next=(\S+)') { $Matches[1] }
        } | Select-Object -Unique)

    Assert-Contains 'rotation' '轮换遇到坏模型时提示失败原因' $lines 'Model switch failed and rolled back: previous=\S+ next=keyboard'
    Assert-True 'rotation' '没有卡死在"正在切换" (无 30s 超时)' `
        (-not (@($lines | Select-String 'Model loading timed out').Count)) `
        ("timed-out lines: " + @($lines | Select-String 'Model loading timed out').Count)
    Assert-Contains 'rotation' '运行期间未崩溃 (应用自己的 shutdown 走完)' $lines 'Shutdown complete: exit_code=0'
    Assert-True 'rotation' '轮换确实在推进 (至少 3 次切换动作)' ($rotations.Count -ge 3) `
        ("rotation events={0} reached={1}" -f $rotations.Count, ($reached -join ','))
    Assert-True 'rotation' '坏模型没有让轮换永远停在同一格 (到达过坏模型之外的两个模型)' `
        ($reached.Count -ge 2) ("reached models: " + ($reached -join ','))
    if ($frames) {
        Assert-True 'rotation' '主循环持续出帧' ($frames.Frames -ge 300) ("frames={0}" -f $frames.Frames)
        Assert-True 'rotation' "最大帧间隔 <= $MaxFrameGapMs ms" ($frames.MaxMs -le $MaxFrameGapMs) `
            ("max_ms={0} p99_ms={1} median_ms={2}" -f $frames.MaxMs, $frames.P99Ms, $frames.MedianMs)
    } else {
        Assert-True 'rotation' '主循环持续出帧' $false 'state/frame-interval.txt missing'
    }
    $script:Evidence.Add($shot)
    $script:Evidence.Add((Join-Path $storage 'state\frame-interval.txt'))
    $script:Logs.Add((Join-Path $storage 'logs\BongoCat.log'))
    Write-Host ("  rotation events={0} reached={1} exit={2}" -f $rotations.Count, ($reached -join ','), $exit)
}

# 场景 4: 副宠。主进程按 session 拉起子进程, 通过 state/pets/<hash>/control.txt 下发状态。
function Invoke-Secondary {
    param([switch]$BadModel)
    $name = if ($BadModel) { 'secondary-bad' } else { 'secondary' }
    Write-Host "== 场景 $name ($Backend) =="
    $storage = New-Storage "$name-$Backend"
    Write-Settings -Storage $storage -Patch @{ 'rendering.multiplePets' = $true } | Out-Null
    Install-Builtins -Storage $storage -Tag "$name-warm"
    if ($BadModel) { Set-Corrupt -Storage $storage -Model 'keyboard' -Kind 'bad-json' }
    # 副模型写在 session.json 里: 主进程据此拉起子进程。
    $session = @{
        format = 'bongocat/session'; schemaVersion = 1
        window = @{ visible = $true; scalePercent = 75.0; opacityPercent = 100.0 }
        activeModelId = 'standard'
        additionalModelIds = @('keyboard')
        activeBehaviors = @()
    }
    $session | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $storage 'state\session.json') -Encoding utf8
    $run = Start-Pet -Storage $storage -Tag "$name-$Backend" -InstanceId ("e2e$($name -replace '-','')") -Extra @(
        '--ci-frame-series', "--ci-exit-ms=$SecondaryExitMs")
    Start-Sleep -Seconds 6
    $childCount = @(Get-CimInstance Win32_Process -Filter "ParentProcessId=$($run.Process.Id)" `
            -ErrorAction SilentlyContinue).Count
    $petHashDir = Get-ChildItem -Path (Join-Path $storage 'state\pets') -Directory -ErrorAction SilentlyContinue |
        Select-Object -First 1
    $controlPath = if ($petHashDir) { Join-Path $petHashDir.FullName 'control.txt' } else { '' }
    $controlFirst = if ($controlPath -and (Test-Path -LiteralPath $controlPath)) {
        (Get-Item -LiteralPath $controlPath).LastWriteTimeUtc
    } else { $null }
    Start-Sleep -Seconds 4
    $controlSecond = if ($controlPath -and (Test-Path -LiteralPath $controlPath)) {
        (Get-Item -LiteralPath $controlPath).LastWriteTimeUtc
    } else { $null }
    $controlBody = if ($controlPath -and (Test-Path -LiteralPath $controlPath)) {
        (Get-Content -LiteralPath $controlPath -Raw).Trim()
    } else { '' }
    $exit = Stop-Pet -Run $run -TimeoutSeconds ([int]($SecondaryExitMs / 1000) + 40)
    $lines = Get-AppLog -Storage $storage
    $frames = Get-FrameStats -Storage $storage

    Assert-True $name '主进程拉起了副宠子进程' ($childCount -ge 1) ("child processes={0}" -f $childCount)
    Assert-True $name '控制文件已下发到 state/pets/<hash>/control.txt' ($controlBody -ne '') `
        ("path={0} body='{1}'" -f $controlPath, $controlBody)
    Assert-True $name '控制文件按心跳续写 (mtime 前进)' `
        ($controlFirst -and $controlSecond -and $controlSecond -gt $controlFirst) `
        ("mtime {0} -> {1}" -f $controlFirst, $controlSecond)
    # 子进程的日志来源标签是 [pet:<模型 id>] (见 log 的 category/tag 约定), 不是 [<id>]。
    Assert-True $name '子进程按自己的模型启动 (副模型 id 出现在日志来源里)' `
        (@($lines | Select-String '\[pet:keyboard\]').Count -ge 1) `
        ("[pet:keyboard] lines=" + @($lines | Select-String '\[pet:keyboard\]').Count)
    Assert-Contains $name '运行期间未崩溃 (应用自己的 shutdown 走完)' $lines 'Shutdown complete: exit_code=0'
    if ($BadModel) {
        # 坏模型在这里走的是**子进程自己的启动兜底**, 与异步切换的回退是两条不同的路径:
        # 子进程发现选中的模型打不开, 就直接加载内置的 standard 并照常启动 (日志里明确写
        # "Selected model was unavailable; loaded fallback model standard")。
        # 注意这**不会**触发多宠的"启动失败摘除"逻辑 —— 那条路径只在子进程反复起不来时走,
        # 而这里子进程活得好好的。所以这里不断言摘除, 断言"回退后子进程仍然可用"。
        Assert-Contains $name '坏模型的子进程读不到 model3.json' $lines `
            '\[pet:keyboard\].*Invalid model setting JSON'
        Assert-Contains $name '坏模型的子进程回退到可用模型 standard' $lines `
            '\[pet:keyboard\].*Selected model was unavailable; loaded fallback model standard'
        Assert-Contains $name '回退后子进程走完启动 (model-ready)' $lines `
            '\[pet:keyboard\].*Startup stage: model-ready'
    }
    if ($frames) {
        Assert-True $name '主进程持续出帧' ($frames.Frames -ge 100) ("frames={0}" -f $frames.Frames)
        Assert-True $name "最大帧间隔 <= $MaxFrameGapMs ms" ($frames.MaxMs -le $MaxFrameGapMs) `
            ("max_ms={0} p99_ms={1} median_ms={2}" -f $frames.MaxMs, $frames.P99Ms, $frames.MedianMs)
    } else {
        Assert-True $name '主进程持续出帧' $false 'state/frame-interval.txt missing'
    }
    $script:Evidence.Add((Join-Path $storage 'state\frame-interval.txt'))
    $script:Logs.Add((Join-Path $storage 'logs\BongoCat.log'))
    Write-Host ("  children={0} control='{1}' exit={2}" -f $childCount, $controlBody, $exit)
}

# ---------------------------------------------------------------------- 主流程
# 场景清单与 'all' 的展开已经在文件开头做完了 (那里还负责校验名字), 这里只管分发。
# 后台更新检查是异步的, 不阻塞主循环, 这里不做特殊处理。

$index = 0
foreach ($item in $Scenario) {
    $index++
    try {
        switch ($item) {
            'bad-json' { Invoke-BadModel -Kind 'bad-json' -Index $index }
            'bad-fields' { Invoke-BadModel -Kind 'bad-fields' -Index $index }
            'bad-moc3' { Invoke-BadModel -Kind 'bad-moc3' -Index $index }
            'bad-moc3-missing' { Invoke-BadModel -Kind 'bad-moc3-missing' -Index $index }
            'bad-texture' { Invoke-BadModel -Kind 'bad-texture' -Index $index }
            'same-model' { Invoke-SameModel }
            'rapid' { Invoke-Rapid }
            'rotation' { Invoke-Rotation }
            'secondary' { Invoke-Secondary }
            'secondary-bad' { Invoke-Secondary -BadModel }
        }
    } catch {
        Add-Result $item "$item 场景执行" $false $_.Exception.Message
    }
}

Write-Host ''
Write-Host '================ 汇总 ================'
$failed = @($script:Results | Where-Object { -not $_.Pass })
$script:Results | Format-Table -AutoSize Scenario, Check, Pass | Out-String -Width 200 | Write-Host
Write-Host ("后端 = {0}; 断言 {1} 条, 失败 {2} 条" -f $Backend, $script:Results.Count, $failed.Count)
Write-Host ("工作目录 = {0}" -f $script:Work)
Write-Host '证据文件:'
foreach ($path in ($script:Evidence + $script:Logs)) { Write-Host ("  {0}" -f $path) }
if ($failed.Count) {
    Write-Host ''
    Write-Host '失败明细:'
    $failed | ForEach-Object { Write-Host ("  {0} / {1}: {2}" -f $_.Scenario, $_.Check, $_.Detail) }
    exit 1
}
exit 0
