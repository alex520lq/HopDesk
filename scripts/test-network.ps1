$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $build | Out-Null
$out = Join-Path $build 'hopdesk-network-tests.exe'
& gcc -std=c11 -Wall -Wextra -I (Join-Path $root 'src/core') -o $out (Join-Path $root 'tests/network_tests.c') (Join-Path $root 'src/core/protocol.c') -lws2_32
if ($LASTEXITCODE -ne 0) { throw "Network test compilation failed ($LASTEXITCODE)" }
& $out
if ($LASTEXITCODE -ne 0) { throw "Network tests failed ($LASTEXITCODE)" }
