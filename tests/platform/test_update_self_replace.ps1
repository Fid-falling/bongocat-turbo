<#
  自动替换链路的验证: "同一个 exe 的隐藏模式 --apply-update" 是否真的能做到
    (等旧进程退出) -> (旧文件改名成 .old) -> (新文件就位) -> (用新文件重启),
  以及失败时是否**不破坏现有安装**。

  为什么用脚本而不是 C 用例: 要验证的是"进程退出之后文件才被换掉"这件事, 必须真的起进程、
  真的等 PID。这里刻意不碰任何用户数据:
    - 全部文件都在临时目录里;
    - "旧版本"是构建产物 BongoCatTurbo.exe 的副本, "新版本"是系统自带 cmd.exe 的副本,
      于是替换成功后的"重启"就是跑一条 cmd 命令, 不会拉起用户的桌宠;
    - 失败场景给 --restart-args=--help: 助手会用旧文件重启, 而 --help 在 SDL/单实例之前
      就返回, 不建窗口、不读配置。

  用法: pwsh -File test_update_self_replace.ps1 -Executable <构建产物绝对路径>
#>
param(
  [Parameter(Mandatory = $true)][string]$Executable,
  [string]$WorkRoot = "",
  # 排查用: 保留临时目录 (默认不管成功失败都清掉, 免得在用户机器上留垃圾)
  [switch]$KeepArtifacts
)

$ErrorActionPreference = 'Stop'
$script:Failures = 0

function Check([bool]$Condition, [string]$Message) {
  if ($Condition) { Write-Host "ok   : $Message" }
  else { Write-Host "FAIL : $Message"; $script:Failures++ }
}

function Get-Sha256([string]$Path) {
  (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

# 助手进程是 GUI 子系统程序, 而且它的参数里有带空格的路径: 必须**自己拼命令行**再启动。
# 不用 Start-Process -ArgumentList: 它会替我们加一层引号, 结果把 --src="..." 弄坏
# (实测会变成 --src=" C:\... ", 助手认不出这个模式, 于是当普通启动跑掉了)。
function Invoke-Applier([string]$Applier, [string]$Arguments, [int]$TimeoutMs = 120000) {
  $Info = New-Object System.Diagnostics.ProcessStartInfo
  $Info.FileName = $Applier
  $Info.Arguments = $Arguments
  $Info.UseShellExecute = $false
  $Process = [System.Diagnostics.Process]::Start($Info)
  if (-not $Process.WaitForExit($TimeoutMs)) {
    $Process.Kill()
    return -1
  }
  return $Process.ExitCode
}

# 与生产代码相同的写法: 路径两侧加引号 (程序可能装在 "C:\Program Files\..." 下)
function Quote([string]$Value) { return '"' + $Value + '"' }

function Common-Arguments([string]$Source, [string]$Target, [string]$Hash, [string]$Log,
  [string]$Result, [string]$Restart, [string]$WaitPid, [string]$WaitMutex = "") {
  $Text = '--apply-update --wait-pid=' + $WaitPid +
    ' --src=' + (Quote $Source) +
    ' --dst=' + (Quote $Target) +
    ' --sha256=' + $Hash +
    ' --restart-args=' + (Quote ($Restart + ' ')) +
    ' --log-file=' + (Quote $Log) +
    ' --result-file=' + (Quote $Result)
  if ($WaitMutex) { $Text += ' --wait-mutex=' + (Quote $WaitMutex) }
  return $Text
}

function New-Case([string]$Root, [string]$Name) {
  $Path = Join-Path $Root $Name
  New-Item -ItemType Directory -Path $Path -Force | Out-Null
  return $Path
}

if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
  Write-Host "FAIL : executable not found: $Executable"
  exit 1
}
if (-not $WorkRoot) {
  $WorkRoot = Join-Path $env:TEMP ('bongocat-self-replace-' + [guid]::NewGuid().ToString('N'))
}
New-Item -ItemType Directory -Path $WorkRoot -Force | Out-Null
Write-Host "work root: $WorkRoot"

$SystemCmd = Join-Path $env:SystemRoot 'System32\cmd.exe'
Check (Test-Path -LiteralPath $SystemCmd) "system cmd.exe is available for the fake new version"

# ---------------------------------------------------------------- 场景 1: 正常替换
$Case1 = New-Case $WorkRoot 'success'
$App = Join-Path $Case1 'app.exe'            # 旧版本, 助手就从这个路径启动 (与真实运行一致)
$New = Join-Path $Case1 'app.exe.new'        # "新版本": 用 cmd.exe 冒充, 重启时可观察
$Marker = Join-Path $Case1 'marker.txt'
$Log = Join-Path $Case1 'apply.log'
$Result = Join-Path $Case1 'result.txt'
Copy-Item -LiteralPath $Executable -Destination $App
Copy-Item -LiteralPath $SystemCmd -Destination $New
$OldHash = Get-Sha256 $App
$NewHash = Get-Sha256 $New

# "旧进程还活着"的信号: 主程序持有这个命名互斥体, 进程一死系统就把它判为 abandoned,
# 助手据此知道可以动手了 (生产实现见 runtime/update/update_install.c)。这里用一个
# 3 秒后自己退出的 powershell 进程扮演"旧程序"。
$MutexName = 'bongocat-self-replace-test-' + [guid]::NewGuid().ToString('N')
# 持有互斥体的"旧进程"写成独立脚本再用 -File 启动: 直接往 -Command 里塞带引号的代码会被
# 命令行拼接弄坏 (和上面 Start-Process -ArgumentList 那个坑是同一个)。
$OwnerScript = Join-Path $Case1 'hold-mutex.ps1'
@'
param([string]$Name, [int]$Seconds = 3)
$m = New-Object System.Threading.Mutex($true, $Name)
Start-Sleep -Seconds $Seconds
$m.ReleaseMutex()
'@ | Set-Content -LiteralPath $OwnerScript -Encoding utf8
$Owner = Start-Process -FilePath 'powershell' -ArgumentList @(
  '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $OwnerScript,
  '-Name', $MutexName, '-Seconds', '3') -PassThru -WindowStyle Hidden
# 等"旧进程"真的把互斥体建起来再启动助手: 互斥体还不存在时助手会正确地判定
# "旧进程已经没了"并立刻动手, 那样这条用例就测不到"等"这个行为。
$Ready = $false
for ($Attempt = 0; $Attempt -lt 50 -and -not $Ready; $Attempt++) {
  try {
    $Probe = [System.Threading.Mutex]::OpenExisting($MutexName)
    $Probe.Dispose()
    $Ready = $true
  } catch { Start-Sleep -Milliseconds 200 }
}
Check $Ready "the fake previous process created its handshake mutex"
$Started = Get-Date
$Exit = Invoke-Applier $App (Common-Arguments $New $App $NewHash $Log $Result `
  ('/c echo updated> ' + $Marker) $Owner.Id $MutexName)
$Elapsed = (Get-Date) - $Started
Check ($Exit -eq 0) "successful replacement exits with 0 (got $Exit)"
Check ($Elapsed.TotalSeconds -ge 2.5) "the helper waited for the old process to exit ($([math]::Round($Elapsed.TotalSeconds,1))s)"
Check ((Get-Sha256 $App) -eq $NewHash) "the target file is now the new version"
Check (Test-Path -LiteralPath ($App + '.old') -PathType Leaf) "the previous version was kept as <exe>.old"
Check ((Get-Sha256 ($App + '.old')) -eq $OldHash) "the .old backup is byte-identical to the previous version"
Check (-not (Test-Path -LiteralPath $New)) "the downloaded file was moved into place, not copied"
$MarkerSeen = $false
for ($Attempt = 0; $Attempt -lt 50 -and -not $MarkerSeen; $Attempt++) {
  if (Test-Path -LiteralPath $Marker -PathType Leaf) { $MarkerSeen = $true; break }
  Start-Sleep -Milliseconds 200
}
Check $MarkerSeen "the new version was restarted (marker file written)"
if ($MarkerSeen) {
  $Text = (Get-Content -LiteralPath $Marker -Raw).Trim()
  Check ($Text -eq 'updated') "the restart used the arguments passed by --restart-args (got '$Text')"
}
Check (-not (Test-Path -LiteralPath $Result) -or
  ((Get-Content -LiteralPath $Result -Raw).Trim() -ne 'failed')) "no failure was reported"

# ------------------------------------------------- 场景 2: 校验不通过 -> 拒绝并清理
$Case2 = New-Case $WorkRoot 'hash-mismatch'
$App2 = Join-Path $Case2 'app.exe'
$New2 = Join-Path $Case2 'app.exe.new'
$Log2 = Join-Path $Case2 'apply.log'
$Result2 = Join-Path $Case2 'result.txt'
Copy-Item -LiteralPath $Executable -Destination $App2
Set-Content -LiteralPath $New2 -Value 'this is not the verified update' -Encoding ascii
$Before2 = Get-Sha256 $App2
$Exit2 = Invoke-Applier $App2 (Common-Arguments $New2 $App2 `
  '0000000000000000000000000000000000000000000000000000000000000000' `
  $Log2 $Result2 '--help' '0')
Check ($Exit2 -ne 0) "a failed verification exits non-zero (got $Exit2)"
Check ((Get-Sha256 $App2) -eq $Before2) "the installed program is untouched"
Check (-not (Test-Path -LiteralPath $New2)) "the rejected file was deleted"
Check (-not (Test-Path -LiteralPath ($App2 + '.old'))) "no backup was created before the file was trusted"
Check ((Test-Path -LiteralPath $Result2) -and
  ((Get-Content -LiteralPath $Result2 -Raw).Trim() -eq 'failed')) "the failure was reported to the next start"

# --------------------------------------------- 场景 3: 目标目录不可写 -> 安全失败
$Case3 = New-Case $WorkRoot 'read-only-target'
$Applier3 = Join-Path $Case3 'applier.exe'   # 助手从别处启动, 因此失败时会重启"自己"
$TargetDir = New-Case $Case3 'install'
$Target3 = Join-Path $TargetDir 'app.exe'
$New3 = Join-Path $Case3 'app.exe.new'
$Log3 = Join-Path $Case3 'apply.log'
$Result3 = Join-Path $Case3 'result.txt'
Copy-Item -LiteralPath $Executable -Destination $Applier3
Copy-Item -LiteralPath $Executable -Destination $Target3
Copy-Item -LiteralPath $SystemCmd -Destination $New3
$Before3 = Get-Sha256 $Target3
$Denied = $false
try {
  # 拒绝当前用户在该目录里写/删 (模拟 Program Files 且未提权)。deny 优先于 allow,
  # 所以即便是目录所有者也会被拦住。
  $null = & icacls $TargetDir /deny "$($env:USERNAME):(WD,AD,DE)" 2>&1
  $Denied = $LASTEXITCODE -eq 0
} catch { $Denied = $false }
Check $Denied "the target directory could be made read-only for this test"
if ($Denied) {
  $Exit3 = Invoke-Applier $Applier3 (Common-Arguments $New3 $Target3 `
    (Get-Sha256 $New3) $Log3 $Result3 '--help' '0')
  Check ($Exit3 -ne 0) "an unwritable target exits non-zero (got $Exit3)"
  Check ((Get-Sha256 $Target3) -eq $Before3) "the installed program is untouched"
  Check (Test-Path -LiteralPath $New3) "the verified file is kept so the user can replace it manually"
  Check ((Test-Path -LiteralPath $Result3) -and
    ((Get-Content -LiteralPath $Result3 -Raw).Trim() -eq 'failed')) "the failure was reported to the next start"
  $null = & icacls $TargetDir /remove:d "$($env:USERNAME)" 2>&1
}

# ------------------------------------------------------------------------- 收尾
if (-not $KeepArtifacts -and (Test-Path -LiteralPath $WorkRoot)) {
  Remove-Item -LiteralPath $WorkRoot -Recurse -Force -ErrorAction SilentlyContinue
}
if ($script:Failures) {
  Write-Host "$($script:Failures) checks failed"
  exit 1
}
Write-Host 'update self-replace checks passed'
exit 0
