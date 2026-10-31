$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $build | Out-Null
$out = Join-Path $build 'hopdesk-xfer-protocol-tests.exe'
$inc = Join-Path $root 'src/core'
$sources = @((Join-Path $root 'tests/xfer_protocol_tests.c'), (Join-Path $root 'src/core/protocol.c'), (Join-Path $root 'src/core/crc32.c'), (Join-Path $root 'src/core/xfer_validate.c'))
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /O2 /W4 /I $inc /Fe:$out @sources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -o $out @sources
}
if ($LASTEXITCODE -ne 0) { throw "Xfer protocol test compilation failed ($LASTEXITCODE)" }
& $out
if ($LASTEXITCODE -ne 0) { throw "Xfer protocol tests failed ($LASTEXITCODE)" }
