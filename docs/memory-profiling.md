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

On macOS the experimental CUI build runs as two processes by default when the main window is open: the UI process and a background process launched with the --agent argument. The transfer engine, network loop, and transfer tracking memory run in the agent; the UI process owns the visible page and its presentation state. Profiling only the UI would miss most transfer-engine memory.

Find both processes:

    pgrep -fl HopDesk

For a full picture, run one sampler for the UI PID and another for the --agent PID at the same time. Replace the example PIDs with the values shown by pgrep:

    bash scripts/profile-memory-macos.sh --pid UI_PID --interval 1 --duration 300 --output /tmp/hopdesk-ui-idle.csv &
    UI_PROFILER=$!
    bash scripts/profile-memory-macos.sh --pid AGENT_PID --interval 1 --duration 300 --output /tmp/hopdesk-agent-idle.csv &
    AGENT_PROFILER=$!
    wait "$UI_PROFILER" "$AGENT_PROFILER"

Repeat with distinct output names for each scenario, such as hopdesk-ui-many-files.csv and hopdesk-agent-many-files.csv. Each process capture also creates a .vmmap.txt summary when the process is still running.

A normal first launch without arguments starts the agent (as does explicit --agent); the tray starts the independent --ui process on demand. With the window closed only the agent should remain. The UI must exit after its CUI loop returns and any requested settings save settles. A normal no-argument agent has no --agent suffix; verify its PID by keeping the tray open while closing the UI. macOS AutoFill is an OS helper, not the HopDesk IPC agent; record it separately and verify whether it disappears after UI exit. Do not sum RSS as unique physical memory.

For the lifecycle regression: record the agent PID and Activity Monitor Memory before the first UI open, open/close 20 times, and after each close verify the UI PID exits while the agent PID remains unchanged. Repeat while connected and during a file transfer; verify tray progress and completion notification, and close immediately after pressing Save to check persistence. Record the helper PID and whether it exits within 30–60 seconds. Compare agent memory against its own pre-open baseline, not the UI peak. Split mode currently requires manual application updates: stop the agent safely from the menu, then install the new build.

When switching between baseline and current builds, quit HopDesk and unload the LaunchAgent before starting the other app bundle. Otherwise launchd may keep the previous agent executable loaded:

    launchctl bootout "gui/$(id -u)/com.hopdesk.split-agent" 2>/dev/null || true

After launching the next build, run pgrep -fl HopDesk again and verify that the --agent process points to the expected app bundle.

## Windows

The experimental CUI Windows build runs the tray/engine agent and an on-demand UI child. Find both processes with:

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

### macOS Metal drawable A/B comparison (experimental)

The default build caps CAMetalLayer.maximumDrawableCount at 2, retaining native Retina resolution, the same UI size and the upstream three vertex/index upload slots. The latter are separate from full-window drawable surfaces. The build generates an adjusted source in build/ and does not modify the pinned CUI submodule. The override fails closed if the upstream initialization changes.

Build the control with `HOPDESK_METAL_DRAWABLE_COUNT=3 sh scripts/build-mac.sh` and the experiment with `HOPDESK_METAL_DRAWABLE_COUNT=2 sh scripts/build-mac.sh`. Copy each package before the next build overwrites it. Compare identical window sizes, display scaling, page, time since opening, Activity Monitor Memory and vmmap summaries for the UI PID. At 1180 × 820 points and 2× backing scale, one BGRA8 surface has approximately 14.8 MiB of pixels; this is a theoretical capacity difference, not a measured reduction. Test repeated resizes, scroll, text input, transfer updates, and 20 open/close cycles. Revert to three drawables if double buffering introduces stalls or rendering defects. Keep the agent PID and its memory baseline separate.

### Windows process isolation verification

Use Task Manager Details or `Get-CimInstance Win32_Process -Filter "Name='HopDesk.exe'" | Select ProcessId,ParentProcessId,CommandLine` to distinguish the agent from its `--ui` child. Record WorkingSet64 and PrivateMemorySize64 separately. Start with only the agent, open the UI from the tray, then close the UI and verify that its PID exits while the agent PID and connection remain stable. Repeat 20 times, including an active transfer; exercise cancel, retry, page changes, reconnect, display, settings save followed immediately by close, audio hot-apply, logs and copy. Verify the agent exits its UI child on tray exit and update. Compare idle agent memory to its initial baseline. No Windows memory reduction is claimed until measured.
