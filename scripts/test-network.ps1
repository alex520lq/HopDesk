$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $build | Out-Null
$out = Join-Path $build 'hopdesk-network-tests.exe'
$inc = Join-Path $root 'src/core'
$sources = @((Join-Path $root 'tests/network_tests.c'), (Join-Path $root 'src/core/protocol.c'))
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /O2 /W4 /utf-8 /D_CRT_SECURE_NO_WARNINGS /I $inc /Fe:$out @sources ws2_32.lib
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -o $out @sources -lws2_32
}
if ($LASTEXITCODE -ne 0) { throw "Network test compilation failed ($LASTEXITCODE)" }
& $out
if ($LASTEXITCODE -ne 0) { throw "Network tests failed ($LASTEXITCODE)" }

$out = Join-Path $build 'hopdesk-audio-link-tests.exe'
$sources = @('tests/audio_link_tests.c', 'src/platform/audio_link.c', 'src/core/audio_protocol.c',
             'src/core/protocol.c', 'src/core/session.c', 'src/core/clock.c', 'src/core/log.c') |
    ForEach-Object { Join-Path $root $_ }
if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe /nologo /std:c11 /O2 /W4 /utf-8 /D_CRT_SECURE_NO_WARNINGS /I $inc /Fe:$out @sources ws2_32.lib
} else {
    & gcc -std=c11 -O2 -Wall -Wextra -I $inc -o $out @sources -lws2_32
}
if ($LASTEXITCODE -ne 0) { throw "Audio link test compilation failed ($LASTEXITCODE)" }
& $out
if ($LASTEXITCODE -ne 0) { throw "Audio link tests failed ($LASTEXITCODE)" }
