# HopDesk transfer memory profiling

Use the platform profiler to capture the HopDesk process memory curve while manually exercising one scenario. Run each scenario separately and use the same build and workload when comparing results.

## Scenarios

Capture one CSV per scenario:

- hopdesk-idle.csv: app launched, no transfer page activity
- hopdesk-transfer-page.csv: open the Transfer page and leave it visible
- hopdesk-many-files.csv: run a large file-count transfer
- hopdesk-completed.csv: finish a transfer and observe memory settling for 30–60 seconds

Use a 300-second capture for each scenario. Start the capture, then exercise only that scenario during the run.

## macOS

Find the UI process. If the split agent is running too, more than one process may be named HopDesk:

    pgrep -fl HopDesk

Use the PID for the main HopDesk UI process, not the background --agent process.

From the repository root:

    bash scripts/profile-memory-macos.sh --pid UI_PID --interval 1 --duration 300 --output /tmp/hopdesk-idle.csv

Repeat with a different output filename for each scenario. Each run also creates a .vmmap.txt summary beside the CSV when the process is still running.

## Windows

Find the UI process:

    Get-Process -Name HopDesk | Select-Object Id, ProcessName, WorkingSet64, PrivateMemorySize64, VirtualMemorySize64

If exactly one HopDesk process is running, the script can find it automatically. Otherwise, pass its PID explicitly:

    pwsh -NoProfile -File scripts/profile-memory-windows.ps1 -ProcessId 1234 -IntervalSeconds 1 -DurationSeconds 300 -Output "$env:TEMP\hopdesk-idle.csv"

Repeat with a different output filename for each scenario. The Windows CSV includes working-set RSS, private bytes, and virtual size. Windows does not currently generate a vmmap-style allocation summary.

## Compare the results

Compare:

- idle baseline RSS
- peak RSS while opening the Transfer page
- peak RSS during a many-file transfer
- RSS after completion and after the UI has been idle for 30–60 seconds
- whether memory returns near baseline after repeated transfers

The scripts measure the whole process, not just transfer-owned allocations. macOS RSS and Windows working set are useful resident-memory indicators but are not perfectly identical; compare before/after builds on the same machine and operating system. Windows private_mb is an additional committed-private-memory metric, not a substitute for RSS.

Neither script changes HopDesk settings or terminates the process. Keep the CSV files and share them with the code review so the next optimization is based on measured memory rather than structure-size estimates.
