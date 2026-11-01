# Execute the production updater against real disposable PE files and processes.
# No desktop/network/download operations are performed. UI errors use -Quiet.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$work = Join-Path ([IO.Path]::GetTempPath()) ('hopdesk-update-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
try {
    foreach ($mode in @(0,1,2,3,4)) {
        $exe = Join-Path $work "mode$mode.exe"
        $sources = @((Join-Path $root 'tests/update_child.c'),(Join-Path $root 'src/core/update_state.c'))
        $inc = Join-Path $root 'src/core'
        if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
            & cl.exe /nologo /std:c11 /MT /O2 /W4 /D_CRT_SECURE_NO_WARNINGS "/DTEST_UPDATE_MODE=$mode" /I $inc /Fe:$exe /Fo"$work\" @sources
        } else {
            & gcc -std=c11 -O2 -Wall -Wextra "-DTEST_UPDATE_MODE=$mode" -I $inc -o $exe @sources
        }
        if ($LASTEXITCODE -ne 0) { throw "Update child fixture compilation failed: $mode" }
        # The production installer enforces its normal minimum size, not a test bypass.
        $file = [IO.File]::Open($exe,[IO.FileMode]::Open,[IO.FileAccess]::Write)
        try { if ($file.Length -lt 100KB) { $file.SetLength(100KB) } } finally { $file.Dispose() }
    }
    foreach ($mode in @(0,1,3,4)) {
        $case = Join-Path $work "case$mode"
        $target = Join-Path $case 'target'
        $payload = Join-Path $case 'payload'
        $transaction = Join-Path $case 'transaction'
        New-Item -ItemType Directory -Path $target,$payload,$transaction -Force | Out-Null
        Copy-Item (Join-Path $work 'mode2.exe') (Join-Path $target 'HopDesk.exe')
        $oldHash = (Get-FileHash (Join-Path $target 'HopDesk.exe') -Algorithm SHA256).Hash
        Copy-Item (Join-Path $work "mode$mode.exe") (Join-Path $payload 'HopDesk.exe')
        $archive = Join-Path $case 'update.zip'
        Compress-Archive -Path (Join-Path $payload '*') -DestinationPath $archive
        $helper = Join-Path $case 'helper.ps1'
        # Runtime is Windows PowerShell 5.1, matching the real app launcher.
        $utf8Bom = New-Object Text.UTF8Encoding($true)
        [IO.File]::WriteAllText($helper,[IO.File]::ReadAllText((Join-Path $root 'scripts/update-windows.ps1')),$utf8Bom)
        $pidFile = Join-Path $case 'child.pid'
        $env:HOPDESK_TEST_CHILD_PID = $pidFile
        try {
            & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $helper `
                -Archive $archive -TargetDir $target -ProcessId 2147483647 `
                -Transaction $transaction -Nonce ('a' * 32) -ExpectedVersion '0.5.1' -Quiet
        } finally {
            Remove-Item Env:HOPDESK_TEST_CHILD_PID -ErrorAction SilentlyContinue
        }
        $newHash = (Get-FileHash (Join-Path $target 'HopDesk.exe') -Algorithm SHA256).Hash
        if ($mode -eq 0) {
            if ($newHash -eq $oldHash) { throw 'Healthy update did not install the new executable.' }
        } elseif ($newHash -ne $oldHash) {
            throw "Update mode $mode did not restore the old executable."
        }
        if (Get-ChildItem -LiteralPath $target -Filter '*.previous.*') { throw 'Backup was not finalized/restored.' }
        if (Test-Path -LiteralPath $transaction) { throw 'Transaction leaked.' }
        if (Test-Path -LiteralPath $pidFile) {
            $childId = [int]([IO.File]::ReadAllText($pidFile))
            $process = Get-Process -Id $childId -ErrorAction SilentlyContinue
            if ($process -and $process.Path -eq (Join-Path $target 'HopDesk.exe')) {
                Stop-Process -Id $childId -Force
                $process.WaitForExit(5000) | Out-Null
            }
        }
        Write-Host "PASS: Windows production updater native lifecycle mode $mode"
    }
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
$global:LASTEXITCODE = 0
