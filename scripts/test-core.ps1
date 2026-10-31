$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $build | Out-Null
$out = Join-Path $build 'hopdesk-core-tests.exe'
$sources = @(
    (Join-Path $root 'tests/core_tests.c'),
    (Join-Path $root 'src/core/protocol.c'),
    (Join-Path $root 'src/core/crc32.c'),
    (Join-Path $root 'src/core/config.c'),
    (Join-Path $root 'src/core/keymap.c')
)
& gcc -std=c11 -Wall -Wextra -I (Join-Path $root 'src/core') -o $out @sources
if ($LASTEXITCODE -ne 0) { throw "Core test compilation failed ($LASTEXITCODE)" }
& $out
if ($LASTEXITCODE -ne 0) { throw "Core tests failed ($LASTEXITCODE)" }
