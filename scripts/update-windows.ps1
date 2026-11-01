[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Archive,
    [Parameter(Mandatory = $true)][string]$TargetDir,
    [Parameter(Mandatory = $true)][int]$ProcessId,
    [Parameter(Mandatory = $true)][string]$Transaction,
    [Parameter(Mandatory = $true)][ValidatePattern('^[0-9a-f]{32}$')][string]$Nonce,
    [Parameter(Mandatory = $true)][ValidatePattern('^[0-9]+[.][0-9]+[.][0-9]+$')][string]$ExpectedVersion,
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'
$stage = Join-Path $env:TEMP ("HopDesk-update-" + [guid]::NewGuid().ToString('N'))
$targetExe = Join-Path $TargetDir 'HopDesk.exe'
$incomingExe = Join-Path $TargetDir ('HopDesk.exe.new.' + $Nonce)
$backupExe = Join-Path $TargetDir ('HopDesk.exe.previous.' + $Nonce)
$log = Join-Path $env:TEMP 'HopDesk-update-error.log'
$scriptToDelete = $PSCommandPath
$replacementDone = $false
$healthy = $false
$newProcess = $null
$ready = Join-Path $Transaction 'ready'

function Show-UpdateFailure([string]$Message) {
    $Message | Set-Content -LiteralPath $log -Encoding UTF8
    if ($Quiet) { return }
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
    # Preparation is acknowledged before the old process is asked to exit.
    Set-Content -LiteralPath (Join-Path $Transaction 'prepared.tmp') -Value $Nonce -Encoding ascii
    Move-Item -LiteralPath (Join-Path $Transaction 'prepared.tmp') -Destination (Join-Path $Transaction 'prepared')
    $deadline = [DateTime]::UtcNow.AddSeconds(60)
    while (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue) {
        if ([DateTime]::UtcNow -gt $deadline) {
            throw 'HopDesk 未能在 60 秒内退出，取消安装以避免覆盖正在运行的程序。'
        }
        Start-Sleep -Milliseconds 250
    }

    if (Test-Path -LiteralPath $backupExe) {
        Remove-Item -LiteralPath $backupExe -Force
    }
    if (Test-Path -LiteralPath $targetExe) {
        Move-Item -LiteralPath $targetExe -Destination $backupExe
    }
    $replacementDone = $true # Backup is now authoritative, even if the next move fails.
    Move-Item -LiteralPath $incomingExe -Destination $targetExe

    $env:HOPDESK_UPDATE_READY = $ready
    $env:HOPDESK_UPDATE_NONCE = $Nonce
    $env:HOPDESK_UPDATE_VERSION = $ExpectedVersion
    try {
        $newProcess = Start-Process -FilePath $targetExe -WorkingDirectory $resolvedTarget -PassThru
    } finally {
        Remove-Item Env:HOPDESK_UPDATE_READY,Env:HOPDESK_UPDATE_NONCE,Env:HOPDESK_UPDATE_VERSION -ErrorAction SilentlyContinue
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(45)
    while ([DateTime]::UtcNow -lt $deadline -and !$newProcess.HasExited) {
        if (Test-Path -LiteralPath $ready -PathType Leaf) {
            $lines = @(Get-Content -LiteralPath $ready)
            if ($lines.Count -eq 3 -and $lines[0] -ceq $Nonce -and
                $lines[1] -ceq $ExpectedVersion -and $lines[2] -ceq [string]$newProcess.Id) {
                $healthy = $true
                break
            }
        }
        Start-Sleep -Milliseconds 250
        $newProcess.Refresh()
    }
    if ($healthy) {
        # Receipt proves initialization; require a short survival window as well.
        if ($newProcess.WaitForExit(5000)) { $healthy = $false }
    }
    if (!$healthy) { throw 'New application did not confirm readiness; restoring the previous version.' }
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

    Remove-Item -LiteralPath $backupExe -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $resolvedArchive -Force -ErrorAction SilentlyContinue
} catch {
    $installError = $_.Exception.Message
    if ($replacementDone -and !$healthy) {
        try {
            if ($newProcess -and !$newProcess.HasExited) {
                $newProcess.Kill()
                if (!$newProcess.WaitForExit(5000)) { throw 'Replacement process did not terminate.' }
            }
            if (Test-Path -LiteralPath $targetExe) { Remove-Item -LiteralPath $targetExe -Force }
            Move-Item -LiteralPath $backupExe -Destination $targetExe -Force
            $replacementDone = $false
            Start-Process -FilePath $targetExe -WorkingDirectory $resolvedTarget
        } catch {
            Show-UpdateFailure ("Rollback failed; recover the previous executable from $backupExe. " + $_.Exception.Message)
        }
    }
    Show-UpdateFailure $installError
} finally {
    if (Test-Path -LiteralPath $stage) {
        Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $incomingExe) {
        Remove-Item -LiteralPath $incomingExe -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $Transaction) { Remove-Item -LiteralPath $Transaction -Recurse -Force -ErrorAction SilentlyContinue }
    if ($scriptToDelete -and (Test-Path -LiteralPath $scriptToDelete)) {
        Remove-Item -LiteralPath $scriptToDelete -Force -ErrorAction SilentlyContinue
    }
}
