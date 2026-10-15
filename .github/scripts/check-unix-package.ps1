[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Archive,
    [Parameter(Mandatory = $true)]
    [ValidateSet('linux-x64', 'macos-x64', 'macos-arm64')][string]$Platform,
    [switch]$SkipSmoke,
    # 发行包必须带我们自己的 Live2D 渲染组件 cores/bongo_live2d.*(阶段 2f 起默认要求:
    # 它是 Live2D 的唯一实现, 缺了它包发出去用户就渲染不了)。
    [bool]$ExpectLive2DComponent = $true,
    # 可选: 用户自备 Cubism Core 的路径。给了就在 smoke 之前**按用户的方式**把它投放到
    # <存储根>/cores/<平台>/<架构>/, 于是 smoke 能验证"导入 Core 后真的渲染"(renderer=
    # cubism-native); 没给则只验证"没有 Core 也能起来并如实降级"(renderer=diagnostic)。
    # CI 上 SDK 不一定在场, 所以两种都支持, 而不是假装一定有 Core。
    [string]$CoreLibrary = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$archivePath = (Resolve-Path -LiteralPath $Archive).Path
$testRunner = Join-Path $PSScriptRoot 'test-unix.sh'
$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) ("BongoCatPackage_" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $temporaryRoot | Out-Null
try {
    Push-Location $temporaryRoot
    try {
        cmake -E tar xf $archivePath
        if ($LASTEXITCODE -ne 0) { throw 'Package extraction failed' }
        $roots = @(Get-ChildItem -LiteralPath $temporaryRoot -Directory)
        if ($roots.Count -ne 1 -or $roots[0].Name -notmatch "^BongoCat-[0-9].*-$Platform$") {
            throw 'Expected one production BongoCat package directory'
        }
        $root = $roots[0].FullName
        if ($Platform.StartsWith('macos-')) {
            $executable = Join-Path $root 'BongoCatTurbo.app/Contents/MacOS/BongoCatTurbo'
            $assets = Join-Path $root 'BongoCatTurbo.app/Contents/Resources/assets'
        } else {
            $executable = Join-Path $root 'BongoCatTurbo'
            $assets = Join-Path $root 'assets'
        }
        $required = @($executable) + @(
            'bongocat.png', 'locales/en-US.json',
            'models/standard/cat.model3.json',
            'models/standard/demomodel.moc3',
            'models/standard/demomodel.1024/texture_00.png',
            'FrameworkShaders/VertShaderSrc.vert',
            'FrameworkShaders/FragShaderSrc.frag',
            'FrameworkShaders/VertShaderSrcBlend.vert',
            'FrameworkShaders/FragShaderSrcBlend.frag'
        ) | ForEach-Object {
            if ([IO.Path]::IsPathRooted($_)) { $_ } else { Join-Path $assets $_ }
        }
        foreach ($path in $required) {
            if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or
                (Get-Item -LiteralPath $path).Length -eq 0) {
                throw "Package resource missing or empty: $path"
            }
        }
        $licenses = @(Get-ChildItem -LiteralPath $root -Recurse -File -Filter LICENSE)
        if ($licenses.Count) { throw "Unexpected loose LICENSE files: $($licenses.FullName -join ', ')" }

        # 规则 3(LIVE2D-PLUGIN-PLAN.md §9.1): 发行包内不得出现 Live2D Cubism Core。
        # 第一遍按文件名 —— 覆盖"把 SDK 目录整个拷进包"这类最常见的回归。
        $coreFiles = @(Get-ChildItem -LiteralPath $root -Recurse -File | Where-Object {
                $_.Name -match '^lib?Live2DCubismCore([._-].*)?\.(dll|so|dylib|lib|a)$'
            })
        if ($coreFiles.Count) {
            throw "Core binary leaked into package: $($coreFiles.FullName -join ', ')"
        }
        # 第二遍按**依赖表**扫(而不是按字节里的符号名): 我们的渲染组件里有生成的 shim,
        # 它必然包含 "csmReviveMocInPlace" 这类字符串(要把符号名转交给宿主的 core_symbol),
        # 所以按字节扫会把自己的合法组件误报成 Core。真正有法律含义的事实是
        # "这个二进制链接/加载了 Core", 那在动态依赖表里看得最清楚。
        # 注: 静态链接进 exe 的 Core 不会出现在依赖表里, 它由构建期的链接级守卫
        # (cmake/CubismCompliance.cmake 的 bongo_cat_assert_no_cubism_core_link) 拦。
        $inspector = if ($Platform.StartsWith('macos-')) { 'otool' } else { 'readelf' }
        foreach ($file in Get-ChildItem -LiteralPath $root -Recurse -File) {
            if ($file.Length -lt 4 -or $file.Length -gt 256MB) { continue }
            $head = [IO.File]::ReadAllBytes($file.FullName)[0..3]
            $isElf = $head[0] -eq 0x7f -and $head[1] -eq 0x45 -and $head[2] -eq 0x4c -and $head[3] -eq 0x46
            $isMachO = ($head[0] -eq 0xcf -and $head[1] -eq 0xfa -and $head[2] -eq 0xed -and $head[3] -eq 0xfe) -or
                ($head[0] -eq 0xfe -and $head[1] -eq 0xed -and $head[2] -eq 0xfa -and $head[3] -eq 0xcf) -or
                ($head[0] -eq 0xca -and $head[1] -eq 0xfe -and $head[2] -eq 0xba -and $head[3] -eq 0xbe)
            if (-not ($isElf -or $isMachO)) { continue }
            $dependencies = if ($Platform.StartsWith('macos-')) {
                & $inspector -L $file.FullName 2>$null
            } else {
                & $inspector -d $file.FullName 2>$null
            }
            if ($LASTEXITCODE -ne 0) {
                throw "Cannot inspect package binary '$($file.FullName)' with $inspector"
            }
            $leak = @($dependencies | Where-Object { $_ -match 'Live2DCubismCore' })
            if ($leak.Count) {
                throw "Package binary '$($file.FullName)' depends on the Cubism Core: $($leak -join '; ')"
            }
        }
        Write-Host "Package contains no Live2D Cubism Core: $Platform"

        if ($ExpectLive2DComponent) {
            # 组件是程序自身的一部分, 随包放在 exe 同级的 cores/(见 renderer_module.h)。
            $componentNames = @('cores/libbongo_live2d.so', 'cores/libbongo_live2d.dylib')
            $components = @($componentNames | ForEach-Object { Join-Path $root $_ } |
                Where-Object { Test-Path -LiteralPath $_ -PathType Leaf })
            if ($components.Count -ne 1 -or (Get-Item -LiteralPath $components[0]).Length -eq 0) {
                throw "Package is missing the Live2D renderer component (expected one of: $($componentNames -join ', '))"
            }
            Write-Host "Live2D renderer component present: $($components[0])"
        }
        Write-Host "Package layout verified: $Platform"
        if (-not $SkipSmoke) {
            $storage = Join-Path $temporaryRoot 'smoke-data'
            # 按**用户的方式**投放 Core: <存储根>/cores/<平台>/<架构>/(与程序里的导入流程同一个
            # 落点)。给了 -CoreLibrary 才做这一步 —— 见参数说明。
            $expectNative = $false
            if ($CoreLibrary -and (Test-Path -LiteralPath $CoreLibrary -PathType Leaf)) {
                $platformName = if ($Platform.StartsWith('macos-')) { 'macos' } else { 'linux' }
                $archName = if ($Platform -eq 'macos-arm64') { 'arm64' } else { 'x86_64' }
                $coreDirectory = Join-Path $storage "cores/$platformName/$archName"
                New-Item -ItemType Directory -Path $coreDirectory -Force | Out-Null
                Copy-Item -LiteralPath $CoreLibrary -Destination $coreDirectory -Force
                $expectNative = $true
                Write-Host "Placed the user-supplied Core into $coreDirectory"
            } else {
                Write-Host 'No Core supplied: the smoke verifies the documented degradation instead'
            }
            & bash $testRunner env BONGO_CAT_DISABLE_NEARBY_MODEL_SCAN=1 `
                $executable --ci-smoke --ci-ignore-global-input `
                --ci-live2d-scenario=visual-consistency "--storage-root=$storage"
            if ($LASTEXITCODE -ne 0) { throw 'Packaged application smoke test failed' }
            $audits = @(Get-ChildItem -LiteralPath $storage -Recurse -File -Filter live2d-audit.txt)
            if ($audits.Count -ne 1) { throw 'Packaged application produced no unique Live2D audit' }
            $audit = Get-Content -LiteralPath $audits[0].FullName -Raw
            if ($expectNative) {
                if ($audit -notmatch '(?m)^renderer=cubism-native\r?$' -or
                    $audit -notmatch '(?m)^assertions=passed\r?$') {
                    throw "Packaged Live2D verification failed (expected native rendering): $audit"
                }
                Write-Host "Packaged native Live2D smoke test passed: $Platform"
            } else {
                # 没有 Core: 必须**如实**报告降级 —— 审计里写 cubism-native 才是错的(那说明
                # renderer= 那一行没有跟着运行期事实走)。
                if ($audit -notmatch '(?m)^renderer=diagnostic') {
                    throw "Packaged Live2D verification failed (expected the documented degradation): $audit"
                }
                Write-Host "Packaged degradation smoke test passed (no Core present): $Platform"
            }
        }
    } finally {
        Pop-Location
    }
} finally {
    Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
}
