# =====================================================================
#  HopDesk - Windows 构建脚本
#
#  用法：  pwsh scripts/build-win.ps1 [-Compiler gcc|msvc]
#
#  gcc 优先；若 PATH 无 gcc，会自动探测 D:\w64devkit。
# =====================================================================
[CmdletBinding()]
param(
    [ValidateSet('gcc', 'msvc')][string]$Compiler = 'gcc'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $build | Out-Null

$core = Get-ChildItem -LiteralPath (Join-Path $root 'src/core') -Filter *.c
$win  = Get-ChildItem -LiteralPath (Join-Path $root 'src/win')  -Filter *.c
$src  = @($core.FullName) + @($win.FullName)
$inc  = Join-Path $root 'src/core'
$out  = Join-Path $build 'hopdesk.exe'
$libs = 'ws2_32','dxva2','gdi32','shell32','user32','comctl32'

if ($Compiler -eq 'gcc') {
    $gcc = (Get-Command gcc.exe -ErrorAction SilentlyContinue).Source
    if (-not $gcc) {
        $cand = 'D:\w64devkit\w64devkit\bin\gcc.exe'
        if (Test-Path $cand) { $gcc = $cand }
    }
    if (-not $gcc) { throw '未找到 gcc；可运行 w64devkit，或用 -Compiler msvc。' }

    $libArgs = $libs | ForEach-Object { '-l' + $_ }
    & $gcc -O2 -Wall -Wextra -municode -mwindows -I $inc -o $out $src @libArgs
}
else {
    $cl = (Get-Command cl.exe -ErrorAction SilentlyContinue).Source
    if (-not $cl) { throw '未找到 cl.exe；请在 x64 Native Tools for VS 终端运行。' }
    $libArgs = $libs | ForEach-Object { $_ + '.lib' }
    & cl /nologo /O2 /MT /W3 /I $inc /Fe:$out $src /Fo"$build\\" @libArgs
}

if (Test-Path -LiteralPath $out) {
    $kb = [math]::Round((Get-Item $out).Length / 1KB, 1)
    Write-Host "构建成功: $out  (${kb} KB)" -ForegroundColor Green
}
