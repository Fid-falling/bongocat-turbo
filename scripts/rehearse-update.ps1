<#
  一键更新的安全演练: 在本机起一个假的 release 服务, 把"新版本"指向你指定的 exe
  (默认就是刚构建出来的那个), 于是可以在**完全不碰真实发布**的前提下走完整条
  检查 -> 下载 -> 校验 -> 替换 -> 重启 链路, 包括故意让前几个下载源失败来观察回退。

  它做了什么:
    1. 把 -Executable 复制一份到 <WorkRoot>\app\BongoCatTurbo.exe —— **只能在这份副本上
       演练**, 脚本拒绝直接使用你正在用的那份程序;
    2. 再复制一份当作"线上新版本"的载荷, 算出它的 SHA-256 写进假 release 元数据
       (digest 字段) —— 期望哈希来自元数据这件事与线上完全一致;
    3. 起一个只监听 127.0.0.1 的 HTTP 服务: /feed 返回元数据, 其它路径返回载荷;
    4. 写 <WorkRoot>\data\config\update-sources.txt: 第一个源是连不上的地址 (用来观察
       "正在尝试第 N/M 个源"), 第二个源指向本机服务;
    5. 打印一条可以直接粘到 PowerShell 里运行的启动命令 (带
       BONGO_CAT_UPDATE_FEED_URL 与 --storage-root)。

  演练时应当看到: 关于页按钮变成"下载中 xx%", 窗口底部状态条显示阶段/百分比/源
  1/2 -> 2/2; 校验通过后提示重启, 程序自己退出并在同一位置重新起来 (版本号不变,
  因为载荷就是同一个文件)。停止服务用 Ctrl+C。

  用法:
    pwsh -File scripts/rehearse-update.ps1 -Executable build-vulkan\Release\BongoCatTurbo.exe
#>
param(
  [Parameter(Mandatory = $true)][string]$Executable,
  [string]$WorkRoot = "",
  [int]$Port = 8123
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
  Write-Host "executable not found: $Executable"; exit 1
}
$Executable = (Resolve-Path -LiteralPath $Executable).Path
if (-not $WorkRoot) { $WorkRoot = Join-Path $env:TEMP 'bongocat-update-rehearsal' }
$AppDirectory = Join-Path $WorkRoot 'app'
$DataRoot = Join-Path $WorkRoot 'data'
$ConfigRoot = Join-Path $DataRoot 'config'
$App = Join-Path $AppDirectory 'BongoCatTurbo.exe'
$Payload = Join-Path $WorkRoot 'payload.exe'
New-Item -ItemType Directory -Path $AppDirectory, $ConfigRoot -Force | Out-Null
Copy-Item -LiteralPath $Executable -Destination $App -Force
Copy-Item -LiteralPath $Executable -Destination $Payload -Force
$Hash = (Get-FileHash -LiteralPath $Payload -Algorithm SHA256).Hash.ToLowerInvariant()

# 版本号只要比当前版本大就会进入下载阶段; 资产名必须与客户端按平台拼出来的一致。
$Version = '9999.0.0'
$Platform = if ([Environment]::Is64BitOperatingSystem) { 'windows-x64' } else { 'windows-x86' }
$Asset = "BongoCat-$Version-$Platform-portable.exe"
$Feed = @"
{"tag_name":"v$Version","draft":false,"prerelease":false,
 "html_url":"https://github.com/Fid-falling/bongocat-turbo/releases/tag/v$Version",
 "body":"local rehearsal feed","assets":[
  {"name":"$Asset","digest":"sha256:$Hash","browser_download_url":
   "https://github.com/Fid-falling/bongocat-turbo/releases/download/v$Version/$Asset"}]}
"@

$DeadPort = $Port + 1
$Sources = "# 演练用: 第一个源故意连不上, 第二个才是真的服务`n" +
  "http://127.0.0.1:$DeadPort/`nhttp://127.0.0.1:$Port/`n"
Set-Content -LiteralPath (Join-Path $ConfigRoot 'update-sources.txt') -Value $Sources -Encoding utf8
Remove-Item -LiteralPath (Join-Path $ConfigRoot 'update-sources.state') -Force -ErrorAction SilentlyContinue

$Listener = New-Object System.Net.HttpListener
$Listener.Prefixes.Add("http://127.0.0.1:$Port/")
$Listener.Start()
Write-Host "rehearsal app : $App"
Write-Host "payload hash  : $Hash"
Write-Host "feed          : http://127.0.0.1:$Port/feed"
Write-Host "sources       : $ConfigRoot\update-sources.txt (源 1 不可用 -> 观察回退)"
Write-Host ""
Write-Host "另开一个 PowerShell 窗口执行:" -ForegroundColor Yellow
Write-Host "  `$env:BONGO_CAT_UPDATE_FEED_URL='http://127.0.0.1:$Port/feed'"
Write-Host "  & '$App' --storage-root='$DataRoot'"
Write-Host ""
Write-Host "然后在 设置 -> 关于 里点`"检查更新`"。Ctrl+C 结束演练服务。" -ForegroundColor Yellow

try {
  while ($Listener.IsListening) {
    $Context = $Listener.GetContext()
    $Path = $Context.Request.Url.AbsolutePath
    if ($Path -eq '/feed') {
      $Bytes = [Text.Encoding]::UTF8.GetBytes($Feed)
      $Context.Response.ContentType = 'application/json'
    } else {
      $Bytes = [IO.File]::ReadAllBytes($Payload)
      $Context.Response.ContentType = 'application/octet-stream'
    }
    $Context.Response.ContentLength64 = $Bytes.Length
    $Context.Response.OutputStream.Write($Bytes, 0, $Bytes.Length)
    $Context.Response.OutputStream.Close()
  }
} finally {
  $Listener.Stop()
  Write-Host "rehearsal server stopped; artifacts kept in $WorkRoot"
}
