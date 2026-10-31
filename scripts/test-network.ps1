$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $build | Out-Null
$out = Join-Path $build 'hopdesk-network-tests.exe'
$inc = Join-Path $root 'src/core'
$sources = @((Join-Path $root 'tests/network_tests.c'), (Join-Path $root 'src/core/protocol.c'))
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /O2 /W4 /I $inc /Fe:$out @sources ws2_32.lib
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -o $out @sources -lws2_32
}
if ($LASTEXITCODE -ne 0) { throw "Network test compilation failed ($LASTEXITCODE)" }
& $out
if ($LASTEXITCODE -ne 0) { throw "Network tests failed ($LASTEXITCODE)" }
