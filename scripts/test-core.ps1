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
    (Join-Path $root 'src/core/keymap.c'),
    (Join-Path $root 'src/core/xfer_validate.c'),
    (Join-Path $root 'src/core/update_manifest.c')
)
$inc = Join-Path $root 'src/core'
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /O2 /W4 /utf-8 /D_CRT_SECURE_NO_WARNINGS /I $inc /Fe:$out @sources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -o $out @sources
}
if ($LASTEXITCODE -ne 0) { throw "Core test compilation failed ($LASTEXITCODE)" }
& $out
if ($LASTEXITCODE -ne 0) { throw "Core tests failed ($LASTEXITCODE)" }
& python (Join-Path $root 'scripts/test-regressions.py')
if ($LASTEXITCODE -ne 0) { throw "Targeted regression tests failed ($LASTEXITCODE)" }
$resilience = Join-Path $build 'hopdesk-resilience-tests.exe'
$resilienceSources = @(
    (Join-Path $root 'tests/resilience_tests.c'),
    (Join-Path $root 'src/core/protocol.c'),
    (Join-Path $root 'src/core/clock.c'),
    (Join-Path $root 'src/core/log.c'),
    (Join-Path $root 'src/core/session.c'),
    (Join-Path $root 'src/core/watchdog.c')
)
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /O2 /W4 /utf-8 /D_CRT_SECURE_NO_WARNINGS /I $inc /Fe:$resilience @resilienceSources
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -o $resilience @resilienceSources
}
if ($LASTEXITCODE -ne 0) { throw "Resilience test compilation failed ($LASTEXITCODE)" }
& $resilience (Join-Path $build 'resilience-test.log')
if ($LASTEXITCODE -ne 0) { throw "Resilience tests failed ($LASTEXITCODE)" }
& (Join-Path $PSScriptRoot 'test-ha.ps1')

& $resilience --watchdog-progress
if ($LASTEXITCODE -ne 0) { throw "Progressing watchdog operation unexpectedly failed" }
& $resilience --watchdog-hang
if ($LASTEXITCODE -ne 70) { throw "Watchdog did not fail-stop a stalled operation" }

$global:LASTEXITCODE = 0 # Expected watchdog exit 70 is a successful test.

& python (Join-Path $root 'scripts/test-updates.py')
if ($LASTEXITCODE -ne 0) { throw "Update lifecycle regressions failed" }
& (Join-Path $PSScriptRoot 'test-update-windows.ps1')
