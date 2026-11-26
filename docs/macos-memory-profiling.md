# macOS transfer memory profiling

Use this script to capture the HopDesk process memory curve while manually exercising one scenario. It records resident set size (RSS) and virtual size (VSZ) once per second, then saves a vmmap summary snapshot if the process is still running.

## 1. Find the UI process

If the split agent is running too, more than one process may be named HopDesk:

    pgrep -fl HopDesk

Use the PID for the main HopDesk UI process, not the background --agent process.

## 2. Capture one scenario per CSV

From the repository root:

    bash scripts/profile-memory-macos.sh --pid UI_PID --interval 1 --duration 300 --output /tmp/hopdesk-idle.csv

While it runs, leave HopDesk idle. Repeat with a different output filename for each scenario:

- hopdesk-idle.csv: app launched, no transfer page activity
- hopdesk-transfer-page.csv: open the Transfer page and leave it visible
- hopdesk-many-files.csv: run a large file-count transfer
- hopdesk-completed.csv: finish a transfer and observe memory settling

Each run also creates a .vmmap.txt summary beside the CSV. RSS/VSZ values are reported in MiB.

## 3. Compare the results

Compare:
- idle baseline RSS
- peak RSS while opening the transfer page
- peak RSS during a many-file transfer
- RSS after completion and after the UI has been idle for 30–60 seconds

The script measures the whole process, not just transfer-owned allocations. Compare runs on the same Mac, with the same build and similar workload. It does not change HopDesk settings or terminate the process.
