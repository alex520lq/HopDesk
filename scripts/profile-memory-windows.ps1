[CmdletBinding()]
param(
    [ValidateRange(0, 2147483647)][int]$ProcessId = 0,
    [ValidateNotNullOrEmpty()][string]$ProcessName = 'HopDesk',
    [ValidateRange(0.1, 3600)][double]$IntervalSeconds = 1,
    [ValidateRange(1, 86400)][int]$DurationSeconds = 300,
    [string]$Output = ''
)

$ErrorActionPreference = 'Stop'

if ($ProcessId -gt 0) {
    $target = Get-Process -Id $ProcessId -ErrorAction Stop
    if ($target.ProcessName -ine $ProcessName) {
        throw "PID $ProcessId belongs to '$($target.ProcessName)', not '$ProcessName'."
    }
} else {
    $processes = @(Get-Process -Name $ProcessName -ErrorAction SilentlyContinue)
    if ($processes.Count -eq 0) {
        throw "No process named '$ProcessName' was found. Start HopDesk or pass -ProcessId."
    }
    if ($processes.Count -gt 1) {
        $processes | Select-Object Id, ProcessName | Format-Table -AutoSize | Out-Host
        throw "Multiple '$ProcessName' processes were found; pass -ProcessId explicitly."
    }
    $ProcessId = $processes[0].Id
}

if ([string]::IsNullOrWhiteSpace($Output)) {
    $Output = Join-Path (Get-Location).Path (
        'hopdesk-memory-{0}.csv' -f (Get-Date -Format 'yyyyMMdd-HHmmss')
    )
}
$Output = [System.IO.Path]::GetFullPath($Output)
$outputDirectory = Split-Path -Parent $Output
if (-not [string]::IsNullOrWhiteSpace($outputDirectory)) {
    New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
}

$samples = [System.Collections.Generic.List[object]]::new()
$timer = [System.Diagnostics.Stopwatch]::StartNew()
$sleepMilliseconds = [Math]::Max(1, [int][Math]::Round($IntervalSeconds * 1000))

Write-Host "Profiling $ProcessName (PID $ProcessId) every $IntervalSeconds seconds for $DurationSeconds seconds."
Write-Host "CSV: $Output"
Write-Host "Exercise one scenario during this run; use a separate output file for each scenario."

while ($timer.Elapsed.TotalSeconds -lt $DurationSeconds) {
    $process = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    if (-not $process) {
        Write-Host "Target process exited; stopping."
        break
    }

    $process.Refresh()
    $samples.Add([pscustomobject]@{
        timestamp_utc = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ss.fffZ')
        pid = $ProcessId
        rss_mb = [Math]::Round($process.WorkingSet64 / 1MB, 2)
        private_mb = [Math]::Round($process.PrivateMemorySize64 / 1MB, 2)
        vsz_mb = [Math]::Round($process.VirtualMemorySize64 / 1MB, 2)
        process = $process.ProcessName
    })

    Start-Sleep -Milliseconds $sleepMilliseconds
}

if ($samples.Count -eq 0) {
    throw "No memory samples were recorded for PID $ProcessId."
}

$samples | Export-Csv -LiteralPath $Output -NoTypeInformation -Encoding UTF8
$rss = $samples | Measure-Object -Property rss_mb -Minimum -Maximum -Average
Write-Host ("Recorded {0} samples." -f $samples.Count)
Write-Host ("RSS MiB — min: {0:N2}, peak: {1:N2}, average: {2:N2}" -f $rss.Minimum, $rss.Maximum, $rss.Average)
Write-Host "CSV saved to: $Output"
