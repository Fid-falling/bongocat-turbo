param(
    [Parameter(Mandatory = $true)][string]$ViewerDirectory,
    [Parameter(Mandatory = $true)][string]$NativeDirectory,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [int]$Seed = 20260807,
    [string]$ToolPath = ""
)

if ([string]::IsNullOrWhiteSpace($ToolPath)) {
    $root = Split-Path -Parent $PSScriptRoot
    $candidates = @(Get-ChildItem -LiteralPath $root -Recurse -File `
        -Filter 'cubism_viewer_blind_test.exe' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending)
    if ($candidates.Count -gt 0) { $ToolPath = $candidates[0].FullName }
}
if ([string]::IsNullOrWhiteSpace($ToolPath) -or -not (Test-Path -LiteralPath $ToolPath)) {
    throw "C blind-test executable not found. Build target cubism_viewer_blind_test first."
}

& $ToolPath $ViewerDirectory $NativeDirectory $OutputDirectory $Seed
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
