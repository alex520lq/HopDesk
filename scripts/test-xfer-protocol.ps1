$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $build | Out-Null
$out = Join-Path $build 'hopdesk-xfer-protocol-tests.exe'
$sources = @((Join-Path $root 'tests/xfer_protocol_tests.c'), (Join-Path $root 'src/core/protocol.c'), (Join-Path $root 'src/core/crc32.c'), (Join-Path $root 'src/core/xfer_validate.c'))
& gcc -std=c11 -Wall -Wextra -I (Join-Path $root 'src/core') -o $out @sources
if ($LASTEXITCODE -ne 0) { throw "Xfer protocol test compilation failed ($LASTEXITCODE)" }
& $out
if ($LASTEXITCODE -ne 0) { throw "Xfer protocol tests failed ($LASTEXITCODE)" }
