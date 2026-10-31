# =====================================================================
#  HopDesk - Windows 构建脚本
#
#  用法：  pwsh scripts/build-win.ps1 [-Compiler clang|gcc|msvc]
#
#  clang 优先（与 macOS 端同一前端，警告语义一致）。探测顺序：
#    1. PATH 上的 clang.exe
#    2. winget 安装的 llvm-mingw（MartinStorsjo.LLVM-MinGW.UCRT）
#    3. w64devkit 里的 clang
#  找不到 clang 时回落到 gcc（w64devkit）。
# =====================================================================
[CmdletBinding()]
param(
    [ValidateSet('clang', 'gcc', 'msvc')][string]$Compiler = 'clang'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $build | Out-Null

$core = Get-ChildItem -LiteralPath (Join-Path $root 'src/core') -Filter *.c
$win = Get-ChildItem -LiteralPath (Join-Path $root 'src/win') -Filter *.c
$src = @($core.FullName) + @($win.FullName)
$inc = Join-Path $root 'src/core'
$iconInc = Join-Path $root 'assets/appicon'
$out = Join-Path $build 'hopdesk.exe'
$libs = 'ws2_32', 'dxva2', 'gdi32', 'shell32', 'user32', 'comctl32'
$iconPng = Join-Path $root 'assets/appicon/hopdesk.png'
$iconIco = Join-Path $root 'assets/appicon/hopdesk.ico'
$resource = Join-Path $root 'assets/appicon/hopdesk.rc'
$res = Join-Path $build 'hopdesk-res.res'

if (-not (Test-Path -LiteralPath $iconPng) -or -not (Test-Path -LiteralPath $iconIco)) {
    throw 'Pre-generated app icons are missing. Run scripts/generate-app-icons.py once and commit assets/appicon/hopdesk.png and hopdesk.ico.'
}
Push-Location $root
try {
    if ($Compiler -eq 'msvc') {
        $rc = (Get-Command rc.exe -ErrorAction SilentlyContinue).Source
        if (-not $rc) { throw 'rc.exe not found; initialize the Visual Studio x64 developer environment.' }
        & $rc /nologo /fo $res $resource
        if ($LASTEXITCODE -ne 0) { throw 'MSVC resource compiler failed' }
    } else {
        $windres = (Get-Command windres.exe -ErrorAction SilentlyContinue).Source
        if (-not $windres) { throw 'windres.exe not found; install mingw-w64 binutils' }
        & $windres -i $resource -o $res
        if ($LASTEXITCODE -ne 0) { throw 'windres failed to compile the icon resource' }
    }
} finally { Pop-Location }

function Find-Clang {
    $c = (Get-Command clang.exe -ErrorAction SilentlyContinue).Source
    if ($c) { return $c }

    # winget 的 llvm-mingw：Packages\MartinStorsjo.LLVM-MinGW*\llvm-mingw-*-x86_64\bin\clang.exe
    $pkgs = Join-Path $env:LOCALAPPDATA 'Microsoft/WinGet/Packages'
    $c = Get-ChildItem -LiteralPath $pkgs -Directory -Filter 'MartinStorsjo.LLVM-MinGW*' -ErrorAction SilentlyContinue |
        ForEach-Object { Get-ChildItem -LiteralPath $_.FullName -Directory -Filter 'llvm-mingw-*-x86_64' -ErrorAction SilentlyContinue } |
        ForEach-Object { Join-Path $_.FullName 'bin/clang.exe' } |
        Where-Object { Test-Path -LiteralPath $_ } |
        Select-Object -First 1
    if ($c) { return $c }

    $c = 'D:\w64devkit\w64devkit\bin\clang.exe'
    if (Test-Path -LiteralPath $c) { return $c }
    return $null
}

# -municode 在 clang 的 MinGW driver 里只定义 UNICODE/_UNICODE，
# 不会切换 CRT 入口 —— 必须显式指定 WinMainCRTStartup，
# 否则入口落到 __tmainCRTStartup，__mingw_app_type 保持 0（console）。
$clangFlags = '-municode', '-mwindows', '-Wl,--entry=WinMainCRTStartup'

switch ($Compiler) {
    'clang' {
        $clang = Find-Clang
        if (-not $clang) { throw '未找到 clang；安装 llvm-mingw（winget install MartinStorsjo.LLVM-MinGW.UCRT）或改用 -Compiler gcc。' }

        $libArgs = $libs | ForEach-Object { '-l' + $_ }
        & $clang --target=x86_64-w64-mingw32 -O2 -Wall -Wextra @clangFlags -I $inc -I $iconInc -o $out $src $res @libArgs
        if ($LASTEXITCODE -ne 0) { throw "clang 编译失败（退出码 $LASTEXITCODE）" }
    }
    'gcc' {
        $gcc = (Get-Command gcc.exe -ErrorAction SilentlyContinue).Source
        if (-not $gcc) {
            $cand = 'D:\w64devkit\w64devkit\bin\gcc.exe'
            if (Test-Path -LiteralPath $cand) { $gcc = $cand }
        }
        if (-not $gcc) { throw '未找到 gcc；可运行 w64devkit，或用 -Compiler clang。' }

        $libArgs = $libs | ForEach-Object { '-l' + $_ }
        & $gcc -O2 -Wall -Wextra -municode -mwindows -I $inc -I $iconInc -o $out $src $res @libArgs
        if ($LASTEXITCODE -ne 0) { throw "gcc 编译失败（退出码 $LASTEXITCODE）" }
    }
    'msvc' {
        $cl = (Get-Command cl.exe -ErrorAction SilentlyContinue).Source
        if (-not $cl) { throw '未找到 cl.exe；请在 x64 Native Tools for VS 终端运行。' }
        $libArgs = $libs | ForEach-Object { $_ + '.lib' }
        & cl /nologo /O2 /MT /W3 /I $inc /I $iconInc /Fe:$out /Fo"$build\\" $src $res @libArgs /link /SUBSYSTEM:WINDOWS
        if ($LASTEXITCODE -ne 0) { throw "cl 编译失败（退出码 $LASTEXITCODE）" }
    }
}

if (Test-Path -LiteralPath $out) {
    $kb = [math]::Round((Get-Item -LiteralPath $out).Length / 1KB, 1)
    Write-Host "构建成功: $out  (${kb} KB, $Compiler)" -ForegroundColor Green
}
