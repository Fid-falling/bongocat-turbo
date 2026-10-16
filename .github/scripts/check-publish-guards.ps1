[CmdletBinding()]
param([switch]$SelfTest)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$upstreamGuards = @(
    "github.repository == 'vladelaina/BongoCat'"
)
$workflowDirectory = Join-Path (Split-Path $PSScriptRoot -Parent) 'workflows'
$sensitiveMarkers = @(
    'build-store-package.ps1',
    'CUBISM_SDK_ARCHIVE_URL',
    'VIRUSTOTAL_API_KEY',
    'actions/upload-artifact',
    'softprops/action-gh-release'
)
$runtimeArtifactMarkers = @(
    'build-store-package.ps1',
    'Upload desktop release',
    'Upload Microsoft Store MSIX',
    'Upload Windows desktop release',
    'Upload Windows portable release',
    'name: bongocat-release-',
    'softprops/action-gh-release'
)
# Live2D Cubism SDK 是专有软件: 只有在被守卫的工作流里才允许还原/编译它, 而应用本身
# **不得**代用户下载 SDK(我们的产品决策见 LIVE2D-PLUGIN-PLAN.md §14 排序 4: 应用内代下载
# 法律上更激进, v1 不做)。两条都做成可执行策略, 免得后来"顺手加个下载按钮"就破功。
$cubismSdkRestoreWorkflows = @('release.yml', 'ci.yml')
$cubismSdkFetchMarkers = @(
    'restore-cubism-sdk',
    'CUBISM_SDK_ARCHIVE_URL',
    'cubism.live2d.com',
    'CubismSdkForNative-'
)
# 应用源码里出现**除官方页面主机之外**的 live2d.com = 有人写了"程序自己下载官方 SDK/Core"的
# 代码路径。这条判据从"出现域名即失败"收紧/放宽成下面这一对, 因为引导用户去官网这件事本身
# 就需要一个官方链接:
#   * **允许** www.live2d.com —— 官方**页面**主机。应用只用 SDL_OpenURL 把用户送到那个页面,
#     由用户自己在官网上阅读并同意许可, 这是唯一合规的取得方式;
#   * **禁止** 其它 live2d.com 主机(典型是 cubism.live2d.com) —— 那是官方压缩包的**直链**主机,
#     只有"程序代下载"才会用到, 正常路径(打开页面 / 读用户自己选中的文件)一个都不需要;
#   * **禁止** 代下载的实现特征(见 $cubismDownloadLogicMarkers);
#   * **禁止** 把 Core 二进制放进源码树/资源树(见 Get-EmbeddedCoreFailures)。
# 下面这个判据因此不再"看到域名就报", 而是"摘掉允许的主机后还剩下 live2d.com 就报"。
$cubismAllowedPageHost = 'www.live2d.com'
# "程序自己下载"的代码路径特征: 代下载实现才会出现, 打开页面与读用户选的文件都不需要。
$cubismDownloadLogicMarkers = @(
    'CUBISM_SDK_ARCHIVE_URL',
    'Live2DCubismCore.zip'
)
# Core 二进制的文件名特征(与 cmake/CubismCompliance.cmake 的判定一致)。
# 写成 `^(lib)?` 而不是 `^lib?`: 后者是"li + 可选的 b", 于是永远匹配不到裸名
# Live2DCubismCore.dll (最可能在 Windows 上泄漏的那个名字), 只能匹配 libLive2DCubismCore.so。
# -SelfTest 里的"内嵌 Core"反例就是为了让这个洞不会再回来。
$cubismCoreFilePattern = '^(lib)?Live2DCubismCore([._-].*)?\.(dll|so|dylib|lib|a)$'

# 某个工作流是否偷偷还原/下载了 SDK。抽成函数是为了让 -SelfTest 能拿反例验证它真的会拒。
function Get-CubismRestoreFailures {
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][string]$Name
    )
    if ($cubismSdkRestoreWorkflows -contains $Name) { return @() }
    $matched = @($cubismSdkFetchMarkers | Where-Object { $Text.Contains($_) })
    if ($matched.Count -eq 0) { return @() }
    return @("$Name`: restores or downloads the proprietary Live2D Cubism SDK; only " +
        "$($cubismSdkRestoreWorkflows -join '/') may do that " +
        "(matched $($matched -join ', '))")
}

function Get-PublishingGuardFailures {
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $jobsStart = $Text.IndexOf("jobs:", [StringComparison]::Ordinal)
    if ($jobsStart -lt 0) { return @() }
    $jobsText = $Text.Substring($jobsStart)
    $jobPattern = '(?ms)^  ([A-Za-z0-9_-]+):\s*\r?\n' +
        '(.*?)(?=^  [A-Za-z0-9_-]+:\s*\r?\n|\z)'
    $failures = @()
    $jobMatches = [regex]::Matches($jobsText, $jobPattern)
    $buildJob = @($jobMatches | Where-Object {
        $_.Groups[1].Value -eq 'build'
    } | Select-Object -First 1)
    $cubismRequirementMarkers = @(
        '-DBONGO_CAT_REQUIRE_CUBISM=ON',
        '-RequireCubism'
    )
    $protectedBuild = $buildJob.Count -eq 1 -and @(
        $cubismRequirementMarkers | Where-Object {
            $buildJob[0].Value.Contains($_)
        }
    ).Count -gt 0

    foreach ($job in $jobMatches) {
        $jobName = $job.Groups[1].Value
        $jobText = $job.Value
        $matched = @($sensitiveMarkers | Where-Object { $jobText.Contains($_) })
        if ($matched.Count -eq 0) { continue }
        $guarded = @($upstreamGuards | Where-Object {
            $jobText.Contains($_)
        }).Count -gt 0
        if (-not $guarded) {
            $failures += "$Label`: job '$jobName' lacks upstream guard; " +
                "matched $($matched -join ', ')"
        }
        $publishesRuntimeArtifacts = @($runtimeArtifactMarkers | Where-Object {
            $jobText.Contains($_)
        }).Count -gt 0
        $usesProtectedBuild = $protectedBuild -and $jobText -match
            '(?m)^\s+needs:\s*(?:build|\[[^\]]*\bbuild\b[^\]]*\])\s*$'
        $hasCubismRequirement = @(
            $cubismRequirementMarkers | Where-Object {
                $jobText.Contains($_)
            }
        ).Count -gt 0
        if ($publishesRuntimeArtifacts -and
            -not $hasCubismRequirement -and
            -not $usesProtectedBuild) {
            $failures += "$Label`: job '$jobName' publishes artifacts without " +
                'requiring a Cubism SDK build'
        }
    }
    return $failures
}

$workflowPaths = @(
    Get-ChildItem -LiteralPath $workflowDirectory -File |
        Where-Object { $_.Extension -in @('.yml', '.yaml') } |
        Sort-Object FullName
)
$failures = @()
$checkedJobs = 0
$selfTestText = $null

foreach ($path in $workflowPaths) {
    $text = Get-Content -LiteralPath $path.FullName -Raw
    $pathFailures = @(Get-PublishingGuardFailures -Text $text -Label $path.Name)
    $failures += $pathFailures
    if (-not ($cubismSdkRestoreWorkflows -contains $path.Name)) {
        $failures += @(Get-CubismRestoreFailures -Text $text -Name $path.Name)
    }
    $jobsStart = $text.IndexOf("jobs:", [StringComparison]::Ordinal)
    if ($jobsStart -ge 0) {
        $jobsText = $text.Substring($jobsStart)
        $checkedJobs += @([regex]::Matches($jobsText,
            '(?ms)^  ([A-Za-z0-9_-]+):\s*\r?\n(.*?)(?=^  [A-Za-z0-9_-]+:\s*\r?\n|\z)') |
            Where-Object {
                $jobText = $_.Value
                @($sensitiveMarkers | Where-Object {
                    $jobText.Contains($_)
                }).Count -gt 0
            }).Count
    }
    if (-not $selfTestText -and
        ($sensitiveMarkers | Where-Object { $text.Contains($_) })) {
        $selfTestText = $text
    }
}

# 应用源码侧的策略: 不得出现"程序自己下载官方 SDK/Core"的代码路径, 也不得内嵌 Core 二进制。
# 注意这里**不是**"出现 live2d.com 即失败": 打开官方页面是允许的用途(见上面的说明), 判据是
# "允许的页面主机摘掉之后, 还有没有别的 live2d.com 主机"。
function Get-InAppDownloadFailures {
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][string]$Label
    )
    $failures = @()
    if ($Text.Replace($cubismAllowedPageHost, '').Contains('live2d.com')) {
        $failures += "$Label references a live2d.com host other than " +
        "$cubismAllowedPageHost`: only the official *page* may be opened (with " +
        'SDL_OpenURL); downloading the proprietary Cubism SDK/Core from a direct ' +
        'host is not allowed'
    }
    $matched = @($cubismDownloadLogicMarkers | Where-Object { $Text.Contains($_) })
    if ($matched.Count -gt 0) {
        $failures += "$Label contains in-app download logic for the proprietary " +
        "Cubism SDK (matched $($matched -join ', '))"
    }
    return $failures
}

# 源码树/资源树里不得出现 Core 二进制: 内嵌一份等于随包分发专有库。
function Get-EmbeddedCoreFailures {
    param([Parameter(Mandatory = $true)][string]$Root)
    $failures = @()
    foreach ($tree in 'src', 'resources') {
        $directory = Join-Path $Root $tree
        if (-not (Test-Path -LiteralPath $directory)) { continue }
        $found = @(Get-ChildItem -LiteralPath $directory -Recurse -File |
            Where-Object { $_.Name -match $cubismCoreFilePattern })
        if ($found.Count -gt 0) {
            $failures += "embedded Cubism Core binary in $tree/: $($found.FullName -join ', ')"
        }
    }
    return $failures
}

$sourceRoot = Join-Path (Split-Path $PSScriptRoot -Parent) '..'
$sourceFiles = @(Get-ChildItem -LiteralPath (Join-Path $sourceRoot 'src') -Recurse -File |
    Where-Object { $_.Extension -in @('.c', '.cc', '.cpp', '.h', '.hpp', '.mm') })
foreach ($file in $sourceFiles) {
    $failures += @(Get-InAppDownloadFailures -Text (Get-Content -LiteralPath $file.FullName -Raw) `
        -Label ("src/" + $file.Name))
}
$failures += @(Get-EmbeddedCoreFailures -Root $sourceRoot)

if ($SelfTest) {
    if (-not $selfTestText) {
        $failures += 'Self-test could not find a sensitive workflow.'
    }
    else {
        $mutated = $selfTestText
        foreach ($guard in $upstreamGuards) {
            $mutated = $mutated.Replace($guard, 'true')
        }
        $caught = @(Get-PublishingGuardFailures -Text $mutated -Label 'self-test')
        if ($caught.Count -eq 0) {
            $failures += 'Self-test did not reject a removed upstream guard.'
        }
        $withoutCubism = $selfTestText.Replace(
            '-DBONGO_CAT_REQUIRE_CUBISM=ON',
            '-DBONGO_CAT_REQUIRE_CUBISM=OFF').Replace(
            '-RequireCubism', '')
        $cubismFailures = @(Get-PublishingGuardFailures `
            -Text $withoutCubism -Label 'self-test')
        if (-not ($cubismFailures -match 'requiring a Cubism SDK build')) {
            $failures += 'Self-test did not reject an SDK-free artifact job.'
        }
        # 反例 1: 一个不被允许的工作流里出现还原 SDK 的 marker, 必须被拒。
        $leakedRestore = $selfTestText + "`n      - run: ./restore-cubism-sdk.ps1`n"
        $restoreCaught = @(Get-CubismRestoreFailures -Text $leakedRestore `
            -Name 'untrusted.yml')
        if ($restoreCaught.Count -eq 0) {
            $failures += 'Self-test did not reject a Cubism SDK restore in an untrusted workflow.'
        }
        # 反例 2: 应用源码里出现 SDK 压缩包的**直链主机**, 必须被拒。
        $downloadCaught = @(Get-InAppDownloadFailures `
            -Text "fetch https://cubism.live2d.com/sdk-native/bin/x.zip" `
            -Label 'self-test')
        if ($downloadCaught.Count -eq 0) {
            $failures += 'Self-test did not reject an in-app SDK download.'
        }
        # 正例(反向自检): 打开官方**页面**是允许的用途, 不能被上一条误伤 —— 少了这条,
        # 以后有人把判据改回"看到 live2d.com 就失败"也不会有任何测试报错。
        $pageAllowed = @(Get-InAppDownloadFailures `
            -Text 'SDL_OpenURL("https://www.live2d.com/en/sdk/download/native/")' `
            -Label 'self-test')
        if ($pageAllowed.Count -ne 0) {
            $failures += 'Self-test rejected opening the official Live2D page.'
        }
        # 正例: 同一段文本里再出现别的 live2d.com 主机(直链) 就必须被拒。
        $pagePlusDirect = @(Get-InAppDownloadFailures `
            -Text ("https://www.live2d.com/en/sdk/download/native/" +
                " and https://cubism.live2d.com/sdk-native/bin/x.zip") `
            -Label 'self-test')
        if ($pagePlusDirect.Count -eq 0) {
            $failures += 'Self-test allowed a direct download host next to the page host.'
        }
        # 反例 3: 代下载的实现特征, 必须被拒。
        $logicCaught = @(Get-InAppDownloadFailures `
            -Text 'const char *url = CUBISM_SDK_ARCHIVE_URL;' -Label 'self-test')
        if ($logicCaught.Count -eq 0) {
            $failures += 'Self-test did not reject in-app download logic.'
        }
        # 内嵌 Core 二进制的自检: 用临时目录造一份假 Core, 命中才算过。
        $coreProbe = Join-Path ([IO.Path]::GetTempPath()) ("BongoCatCoreProbe_" + [guid]::NewGuid())
        New-Item -ItemType Directory -Path $coreProbe | Out-Null
        New-Item -ItemType Directory -Path (Join-Path $coreProbe 'src') | Out-Null
        Set-Content -LiteralPath (Join-Path $coreProbe 'src/Live2DCubismCore_MT.lib') -Value 'x'
        try {
            $embeddedCaught = @(Get-EmbeddedCoreFailures -Root $coreProbe)
            if ($embeddedCaught.Count -eq 0) {
                $failures += 'Self-test did not reject an embedded Core binary.'
            }
        }
        finally {
            Remove-Item -LiteralPath $coreProbe -Recurse -Force
        }
    }
}

if ($failures.Count -gt 0) {
    Write-Error ("Publishing guard policy failed:`n- " +
        ($failures -join "`n- "))
    exit 1
}

Write-Host "Publishing guard policy passed for $checkedJobs sensitive job(s)."
if ($SelfTest) { Write-Host 'Publishing guard negative self-tests passed.' }
