# =====================================================================
#  HopDesk - Windows 构建脚本
#
#  用法：  pwsh scripts/build-win.ps1 [-Compiler msvc|gcc]
#
#  阶段 1 源码就位后即可运行；当前为立项骨架。
# =====================================================================
[CmdletBinding()]
param(
    [ValidateSet('msvc', 'gcc')][string]$Compiler = 'msvc'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $build | Out-Null

$core = Get-ChildItem -LiteralPath (Join-Path $root 'src/core') -Filter *.c -ErrorAction SilentlyContinue
$win  = Get-ChildItem -LiteralPath (Join-Path $root 'src/win')  -Filter *.c -ErrorAction SilentlyContinue
$src  = @($core.FullName) + @($win.FullName)

if (-not $src) {
    Write-Host '暂无 .c 源文件（阶段 1 就位后再构建）。' -ForegroundColor Yellow
    exit 0
}

$out = Join-Path $build 'hopdesk.exe'

if ($Compiler -eq 'gcc') {
    gcc -O2 -Wall -Wextra -o $out $src -lws2_32 -ldxva2 -lgdi32 -lshell32 -luser32
} else {
    Write-Host '请在 "x64 Native Tools for VS" 终端中运行，或确保 cl.exe 在 PATH。'
    cl /nologo /O2 /MT /W3 /Fe:$out $src /Fo"$build\\" ws2_32.lib dxva2.lib gdi32.lib shell32.lib user32.lib
}

if (Test-Path -LiteralPath $out) {
    Write-Host "构建成功: $out" -ForegroundColor Green
}
