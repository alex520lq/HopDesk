#!/usr/bin/env bash
set -euo pipefail

PROCESS_NAME="HopDesk"
PID=""
INTERVAL="1"
DURATION="300"
OUTPUT=""

usage() {
    cat <<'USAGE'
Usage:
  scripts/profile-memory-macos.sh [options]

Options:
  --pid PID          PID of the HopDesk UI process (recommended if the agent is also running)
  --process NAME     Exact process name for automatic lookup (default: HopDesk)
  --interval SECONDS Sampling interval (default: 1)
  --duration SECONDS Sampling duration (default: 300)
  --output FILE      CSV output path (default: ./hopdesk-memory-<timestamp>.csv)
  -h, --help         Show this help

The script records RSS/VSZ once per interval and writes a vmmap summary
beside the CSV when the target process is still running at exit.
USAGE
}

while (($#)); do
    case "$1" in
        --pid)
            [[ $# -ge 2 ]] || { echo "Missing value for --pid" >&2; exit 2; }
            PID="$2"; shift 2 ;;
        --process)
            [[ $# -ge 2 ]] || { echo "Missing value for --process" >&2; exit 2; }
            PROCESS_NAME="$2"; shift 2 ;;
        --interval)
            [[ $# -ge 2 ]] || { echo "Missing value for --interval" >&2; exit 2; }
            INTERVAL="$2"; shift 2 ;;
        --duration)
            [[ $# -ge 2 ]] || { echo "Missing value for --duration" >&2; exit 2; }
            DURATION="$2"; shift 2 ;;
        --output)
            [[ $# -ge 2 ]] || { echo "Missing value for --output" >&2; exit 2; }
            OUTPUT="$2"; shift 2 ;;
        -h|--help)
            usage; exit 0 ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2 ;;
    esac
done

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "This profiler is intended for macOS." >&2
    exit 1
fi

if [[ -z "$PID" ]]; then
    PIDS="$(pgrep -x "$PROCESS_NAME" || true)"
    COUNT="$(printf '%s\n' "$PIDS" | awk 'NF { n++ } END { print n+0 }')"
    if [[ "$COUNT" -eq 1 ]]; then
        PID="$PIDS"
    elif [[ "$COUNT" -eq 0 ]]; then
        echo "No process named '$PROCESS_NAME' was found. Start HopDesk or pass --pid." >&2
        exit 1
    else
        echo "Multiple '$PROCESS_NAME' processes found; pass the UI PID explicitly:" >&2
        pgrep -fl "$PROCESS_NAME" >&2 || true
        exit 1
    fi
fi

if [[ ! "$PID" =~ ^[0-9]+$ ]] || ! kill -0 "$PID" 2>/dev/null; then
    echo "Invalid or non-running PID: $PID" >&2
    exit 1
fi
if [[ ! "$INTERVAL" =~ ^[0-9]+([.][0-9]+)?$ ]] ||
   ! awk -v interval="$INTERVAL" 'BEGIN { exit !(interval > 0) }'; then
    echo "--interval must be a positive number." >&2
    exit 2
fi
if [[ ! "$DURATION" =~ ^[0-9]+$ ]] || [[ "$DURATION" -lt 1 ]]; then
    echo "--duration must be a positive integer." >&2
    exit 2
fi

if [[ -z "$OUTPUT" ]]; then
    OUTPUT="./hopdesk-memory-$(date +%Y%m%d-%H%M%S).csv"
fi
mkdir -p "$(dirname "$OUTPUT")"
SUMMARY="$OUTPUT.vmmap.txt"

write_summary() {
    if kill -0 "$PID" 2>/dev/null && command -v vmmap >/dev/null 2>&1; then
        vmmap -summary "$PID" > "$SUMMARY" 2>&1 || true
    fi
}
trap write_summary EXIT

printf 'timestamp_utc,pid,rss_mb,vsz_mb,process\n' > "$OUTPUT"
echo "Profiling PID $PID every $INTERVAL seconds for $DURATION seconds."
echo "CSV: $OUTPUT"
echo "Exercise one scenario during this run; use a separate output file for each scenario."

END_TIME=$((SECONDS + DURATION))
SAMPLES=0
while (( SECONDS < END_TIME )); do
    STATS="$(ps -p "$PID" -o rss= -o vsz= 2>/dev/null || true)"
    if [[ -z "$STATS" ]]; then
        echo "Target process exited; stopping."
        break
    fi
    read -r RSS_KB VSZ_KB <<< "$STATS"
    if [[ ! "$RSS_KB" =~ ^[0-9]+$ ]] || [[ ! "$VSZ_KB" =~ ^[0-9]+$ ]]; then
        echo "Could not parse process memory for PID $PID: $STATS" >&2
        exit 1
    fi
    RSS_MB="$(awk -v kb="$RSS_KB" 'BEGIN { printf "%.2f", kb / 1024 }')"
    VSZ_MB="$(awk -v kb="$VSZ_KB" 'BEGIN { printf "%.2f", kb / 1024 }')"
    PROCESS="$(ps -p "$PID" -o comm= | tr ',' ' ' | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"
    printf '"%s",%s,%s,%s,"%s"\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$PID" "$RSS_MB" "$VSZ_MB" "$PROCESS" >> "$OUTPUT"
    SAMPLES=$((SAMPLES + 1))
    sleep "$INTERVAL"
done

echo "Recorded $SAMPLES samples."
echo "CSV saved to: $OUTPUT"
if [[ -f "$SUMMARY" ]]; then
    echo "vmmap summary saved to: $SUMMARY"
fi
