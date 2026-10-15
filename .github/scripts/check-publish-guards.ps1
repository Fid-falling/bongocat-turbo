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
# 应用源码里出现这个主机名 = 有人写了"程序自己下载官方 SDK"的代码路径。
$cubismInAppDownloadMarker = 'cubism.live2d.com'

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

# 应用源码侧的策略: 不得出现"程序自己下载官方 SDK"的代码路径(见 LIVE2D-PLUGIN-PLAN.md §14)。
# 引导页只会打开浏览器(www.live2d.com 的下载页), 而 cubism.live2d.com 是官方 SDK 压缩包的
# 直链主机 —— 只有实现代下载才会用到它, 所以它是一条精确的判据而不是泛泛的域名匹配。
function Get-InAppDownloadFailures {
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][string]$Label
    )
    if ($Text.Contains($cubismInAppDownloadMarker)) {
        return @("$Label contains '$cubismInAppDownloadMarker': the application must not " +
            'download the proprietary Cubism SDK itself')
    }
    return @()
}

$sourceRoot = Join-Path (Split-Path $PSScriptRoot -Parent) '..'
$sourceFiles = @(Get-ChildItem -LiteralPath (Join-Path $sourceRoot 'src') -Recurse -File |
    Where-Object { $_.Extension -in @('.c', '.cc', '.cpp', '.h', '.hpp', '.mm') })
foreach ($file in $sourceFiles) {
    $failures += @(Get-InAppDownloadFailures -Text (Get-Content -LiteralPath $file.FullName -Raw) `
        -Label ("src/" + $file.Name))
}

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
        # 反例 2: 应用源码里出现官方 SDK 直链主机, 必须被拒。
        $downloadCaught = @(Get-InAppDownloadFailures `
            -Text "fetch https://$cubismInAppDownloadMarker/sdk-native/bin/x.zip" `
            -Label 'self-test')
        if ($downloadCaught.Count -eq 0) {
            $failures += 'Self-test did not reject an in-app SDK download.'
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
