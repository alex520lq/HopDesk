# User-session supervisor. An intentional normal exit stops supervision.
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Executable)
$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path -LiteralPath $Executable).Path
$delay = 2
while ($true) {
    $started = Get-Date
    $process = Start-Process -FilePath $exe -WorkingDirectory (Split-Path -Parent $exe) -Wait -PassThru
    if ($process.ExitCode -eq 0) { break }
    if (((Get-Date) - $started).TotalSeconds -ge 60) { $delay = 2 }
    Start-Sleep -Seconds $delay
    $delay = [Math]::Min($delay * 2, 30)
}
