# HopDesk memory profiling

Use the platform profiler to capture HopDesk memory while manually exercising one scenario. Run each scenario separately and use the same machine, build settings, and workload for before/after comparisons.

## Scenarios

Capture one CSV per scenario:

- hopdesk-idle.csv: app launched, no transfer page activity
- hopdesk-transfer-page.csv: open the Transfer page and leave it visible
- hopdesk-many-files.csv: run a large file-count transfer
- hopdesk-completed.csv: finish a transfer and observe memory settling for 30–60 seconds

Use a 300-second capture for each scenario. Start the capture, then exercise only that scenario during the run.

## macOS

HopDesk may run as two processes: the UI process and a background process launched with the --agent argument. The transfer engine, network loop, and transfer tracking memory run in the agent; the UI process owns the visible page and its presentation state. Profiling only the UI would miss most transfer-engine memory.

Find both processes:

    pgrep -fl HopDesk

For a full picture, run one sampler for the UI PID and another for the --agent PID at the same time. Replace the example PIDs with the values shown by pgrep:

    bash scripts/profile-memory-macos.sh --pid UI_PID --interval 1 --duration 300 --output /tmp/hopdesk-ui-idle.csv &
    UI_PROFILER=$!
    bash scripts/profile-memory-macos.sh --pid AGENT_PID --interval 1 --duration 300 --output /tmp/hopdesk-agent-idle.csv &
    AGENT_PROFILER=$!
    wait "$UI_PROFILER" "$AGENT_PROFILER"

Repeat with distinct output names for each scenario, such as hopdesk-ui-many-files.csv and hopdesk-agent-many-files.csv. Each process capture also creates a .vmmap.txt summary when the process is still running.

If the split agent is not active and only one HopDesk process exists, profile that process. For transfer-engine memory, the important process is the one that owns xfer_init and the network loop.

When switching between baseline and current builds, quit HopDesk and unload the LaunchAgent before starting the other app bundle. Otherwise launchd may keep the previous agent executable loaded:

    launchctl bootout "gui/$(id -u)/com.hopdesk.split-agent" 2>/dev/null || true

After launching the next build, run pgrep -fl HopDesk again and verify that the --agent process points to the expected app bundle.

## Windows

Windows runs the UI and transfer engine in one HopDesk process. Find it with:

    Get-Process -Name HopDesk | Select-Object Id, ProcessName, WorkingSet64, PrivateMemorySize64, VirtualMemorySize64

If exactly one HopDesk process is running, the script can find it automatically. Otherwise, pass its PID explicitly:

    pwsh -NoProfile -File scripts/profile-memory-windows.ps1 -ProcessId 1234 -IntervalSeconds 1 -DurationSeconds 300 -Output "$env:TEMP\hopdesk-idle.csv"

Repeat with a different output filename for each scenario. The Windows CSV includes working-set RSS, private bytes, and virtual size. Windows does not currently generate a vmmap-style allocation summary.

## Compare the results

Compare:

- idle baseline RSS for the UI and agent separately on macOS
- peak RSS while opening the Transfer page
- peak RSS during a many-file transfer
- RSS after completion and after the UI has been idle for 30–60 seconds
- whether each process returns near its baseline after repeated transfers

The scripts measure whole processes, not just transfer-owned allocations. macOS RSS and Windows working set are useful resident-memory indicators but are not perfectly identical. When looking at both macOS processes together, use the separate curves to identify which process owns the growth; summing RSS is only an approximation because shared pages may be counted in both processes. Windows private_mb is an additional committed-private-memory metric, not a substitute for RSS.

For a before/after comparison of the recent memory optimization series, use baseline commit c95078944d522883d1ac2daf54bd74cb1f871b2b and compare it with the current main build on the same device. You can prepare the baseline in a separate worktree:

    git worktree add /tmp/hopdesk-memory-baseline c95078944d522883d1ac2daf54bd74cb1f871b2b
    cd /tmp/hopdesk-memory-baseline
    sh scripts/build-mac.sh

Build the current main version separately. Run one version at a time, fully quit both HopDesk processes and unload the LaunchAgent between runs, verify which executable owns --agent, and use identical transfer data.

Neither script changes HopDesk settings or terminates the process. Keep the CSV files and vmmap summaries and share them with the code review so the next optimization is based on measured memory rather than structure-size estimates.
