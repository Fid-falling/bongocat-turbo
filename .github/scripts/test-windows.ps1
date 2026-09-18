param(
    [Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)]
    [string[]]$TestCommand
)

$tempRoot = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { '.' }
$log = Join-Path $tempRoot 'bongocat-native-tests.log'
$junit = Join-Path $tempRoot 'bongocat-native-tests.xml'
$executable = $TestCommand[0]
$arguments = if ($TestCommand.Count -gt 1) {
    $TestCommand[1..($TestCommand.Count - 1)]
} else { @() }

& $executable @arguments --output-junit $junit 2>&1 | Tee-Object -FilePath $log
$status = $LASTEXITCODE
if ($status -eq 0) { exit 0 }

$reported = $false
if (Test-Path -LiteralPath $junit) {
    [xml]$report = Get-Content -Raw -LiteralPath $junit
    foreach ($test in @($report.testsuite.testcase)) {
        if (-not $test.failure) { continue }
        $detail = "$($test.failure.InnerText)".Trim()
        if (-not $detail) { $detail = "$($test.failure.message)" }
        $output = "$($test.'system-out')".Trim()
        $message = "$($test.name): $detail (time=$($test.time)s)"
        if ($output) { $message += "`n$output" }
        if ($message.Length -gt 6000) { $message = $message.Substring(0, 6000) }
        $message = $message.Replace('%', '%25').Replace("`r", '%0D').Replace("`n", '%0A')
        Write-Output "::error title=Windows CTest failure::$message"
        $reported = $true
    }
}
if (-not $reported) {
    $message = (Get-Content -LiteralPath $log | Select-Object -Last 60) -join "`n"
    $message = $message.Replace('%', '%25').Replace("`r", '%0D').Replace("`n", '%0A')
    Write-Output "::error title=Windows CTest failure::$message"
}
exit $status
