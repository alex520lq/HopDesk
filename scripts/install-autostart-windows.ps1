# Run as the desktop user, no elevation required. Opt-in only.
[CmdletBinding()]
param([string]$Executable = (Join-Path $PSScriptRoot 'HopDesk.exe'), [switch]$Uninstall)
$ErrorActionPreference = 'Stop'
$name = 'HopDesk user-session supervisor'
if ($Uninstall) {
    Stop-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $name -Confirm:$false -ErrorAction SilentlyContinue
    Write-Host 'HopDesk autostart removed. A currently running HopDesk may be exited from its tray menu.'
    return
}
$exe = (Resolve-Path -LiteralPath $Executable).Path
$supervisor = Join-Path $PSScriptRoot 'supervise-windows.ps1'
if (!(Test-Path -LiteralPath $supervisor)) { throw 'supervise-windows.ps1 must be beside this installer.' }
# The task and executable remain in this directory; do not move/delete it after installation.
$arguments = '-NoProfile -WindowStyle Hidden -File "{0}" -Executable "{1}"' -f $supervisor, $exe
$action = New-ScheduledTaskAction -Execute "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -Argument $arguments
$user = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $user
$principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew
Register-ScheduledTask -TaskName $name -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Force | Out-Null
Write-Host 'Installed. Starts at next sign-in; abnormal exits restart with 2–30s backoff. Normal tray exit stops supervision until next sign-in.'
