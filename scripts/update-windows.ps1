[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Archive,
    [Parameter(Mandatory = $true)][string]$TargetDir,
    [Parameter(Mandatory = $true)][int]$ProcessId
)

$ErrorActionPreference = 'Stop'
$stage = Join-Path $env:TEMP ("HopDesk-update-" + [guid]::NewGuid().ToString('N'))
$targetExe = Join-Path $TargetDir 'HopDesk.exe'
$incomingExe = Join-Path $TargetDir 'HopDesk.exe.new'
$backupExe = Join-Path $TargetDir 'HopDesk.exe.previous'
$log = Join-Path $env:TEMP 'HopDesk-update-error.log'
$scriptToDelete = $PSCommandPath
$replacementDone = $false
$restartStarted = $false

function Show-UpdateFailure([string]$Message) {
    $Message | Set-Content -LiteralPath $log -Encoding UTF8
    try {
        Add-Type -AssemblyName System.Windows.Forms
        [System.Windows.Forms.MessageBox]::Show(
            ($Message + [Environment]::NewLine + [Environment]::NewLine + "更新未能正常完成。请检查日志：$log"),
            'HopDesk 更新失败',
            [System.Windows.Forms.MessageBoxButtons]::OK,
            [System.Windows.Forms.MessageBoxIcon]::Error
        ) | Out-Null
    } catch {
        Write-Error $Message
    }
}

try {
    $resolvedArchive = (Resolve-Path -LiteralPath $Archive).Path
    $resolvedTarget = (Resolve-Path -LiteralPath $TargetDir).Path
    if ([IO.Path]::GetExtension($resolvedArchive) -ne '.zip') {
        throw '更新包不是 ZIP 文件。'
    }

    $deadline = [DateTime]::UtcNow.AddSeconds(60)
    while (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue) {
        if ([DateTime]::UtcNow -gt $deadline) {
            throw 'HopDesk 未能在 60 秒内退出，取消安装以避免覆盖正在运行的程序。'
        }
        Start-Sleep -Milliseconds 250
    }

    New-Item -ItemType Directory -Path $stage -Force | Out-Null
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($resolvedArchive)
    try {
        $allowed = @(
            'HopDesk.exe',
            'update-windows.ps1',
            'install-autostart-windows.ps1',
            'supervise-windows.ps1'
        )
        $total = [long]0
        foreach ($entry in $zip.Entries) {
            if ([string]::IsNullOrEmpty($entry.Name) -or
                $entry.FullName -ne $entry.Name -or
                $allowed -notcontains $entry.Name) {
                throw "更新包包含不允许的路径或文件：$($entry.FullName)"
            }
            if ((($entry.ExternalAttributes -shr 16) -band 0xF000) -eq 0xA000) {
                throw "更新包包含符号链接：$($entry.FullName)"
            }
            $total += $entry.Length
            if ($total -gt 512MB) { throw '更新包解压后超过 512 MB。' }
            $destination = Join-Path $stage $entry.Name
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $destination, $true)
        }
    } finally {
        $zip.Dispose()
    }

    $newExe = Join-Path $stage 'HopDesk.exe'
    if (!(Test-Path -LiteralPath $newExe -PathType Leaf)) {
        throw '更新包中缺少 HopDesk.exe。'
    }
    $newSize = (Get-Item -LiteralPath $newExe).Length
    if ($newSize -lt 100KB -or $newSize -gt 100MB) {
        throw '新程序文件大小异常，拒绝替换。'
    }

    Copy-Item -LiteralPath $newExe -Destination $incomingExe -Force
    if (Test-Path -LiteralPath $backupExe) {
        Remove-Item -LiteralPath $backupExe -Force
    }
    if (Test-Path -LiteralPath $targetExe) {
        Move-Item -LiteralPath $targetExe -Destination $backupExe
    }
    try {
        Move-Item -LiteralPath $incomingExe -Destination $targetExe
        $replacementDone = $true
    } catch {
        if (Test-Path -LiteralPath $backupExe) {
            Move-Item -LiteralPath $backupExe -Destination $targetExe -Force
        }
        throw
    }

    foreach ($name in @('update-windows.ps1', 'install-autostart-windows.ps1', 'supervise-windows.ps1')) {
        $source = Join-Path $stage $name
        if (Test-Path -LiteralPath $source -PathType Leaf) {
            try {
                Copy-Item -LiteralPath $source -Destination (Join-Path $resolvedTarget $name) -Force
            } catch {
                # A failed helper-script refresh must not invalidate the already
                # verified executable; the installed previous helper remains usable.
            }
        }
    }

    Start-Process -FilePath $targetExe -WorkingDirectory $resolvedTarget
    $restartStarted = $true
    Remove-Item -LiteralPath $resolvedArchive -Force -ErrorAction SilentlyContinue
} catch {
    if ($replacementDone -and !$restartStarted -and (Test-Path -LiteralPath $backupExe)) {
        Remove-Item -LiteralPath $targetExe -Force -ErrorAction SilentlyContinue
        Move-Item -LiteralPath $backupExe -Destination $targetExe -Force -ErrorAction SilentlyContinue
        $replacementDone = $false
    }
    if (!(Test-Path -LiteralPath $targetExe) -and (Test-Path -LiteralPath $backupExe)) {
        Move-Item -LiteralPath $backupExe -Destination $targetExe -Force
    }
    Show-UpdateFailure $_.Exception.Message
} finally {
    if (Test-Path -LiteralPath $stage) {
        Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $incomingExe) {
        Remove-Item -LiteralPath $incomingExe -Force -ErrorAction SilentlyContinue
    }
    if ($scriptToDelete -and (Test-Path -LiteralPath $scriptToDelete)) {
        Remove-Item -LiteralPath $scriptToDelete -Force -ErrorAction SilentlyContinue
    }
}
