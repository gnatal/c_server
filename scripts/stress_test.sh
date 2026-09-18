#!/usr/bin/env bash
#
# Stress-tests the Todo CRUD demo (app/) under multi-worker cluster mode
# using wrk. Builds a fresh binary, boots the server in the background,
# seeds a few todos, runs a battery of read + write benchmarks at
# increasing concurrency, then tears everything down.
#
# Memory tracking (on by default, MEASURE_MEMORY=0 to disable):
#   - the server runs under `/usr/bin/time -l` (macOS) / `-v` (Linux), whose
#     report is printed after shutdown. In cluster mode that report's peak
#     RSS is the largest SINGLE process, not the sum across master + workers.
#   - a background sampler polls `ps` for the RSS of master + every worker
#     during each benchmark and reports the peak TOTAL, which is the number
#     that matters for "how much RAM does this whole server use".
#
# Phases (PHASES, space-separated, default "ping churn read write"):
#   ping   GET /ping over keep-alive connections: pure engine throughput, no DB, no JSON
#   churn  GET /ping with "Connection: close": a new TCP connection per request, which
#          stresses accept/close. Every closed connection leaves a TIME_WAIT socket, and in
#          longer runs (observed on macOS at 1000+ conns / 5 s+) wrk starts reporting
#          "connect" errors as they pile up; a 2 s run from a clean state showed none.
#          Shorten DURATION, lower CONNS, or let TIME_WAIT drain between runs
#          (`netstat -an -p tcp | grep -c TIME_WAIT`) before trusting a churn number.
#   read   GET / and GET /api/todos (SQLite read path)
#   write  POST /api/todos (SQLite write path)
#
# Usage: scripts/stress_test.sh
#        PHASES=ping scripts/stress_test.sh          # connection stress only
#        PHASES="ping churn" CONNS="1000 5000" DURATION=10s scripts/stress_test.sh
# Tunable via env vars: PORT, WORKERS, THREADS, DURATION, CONNS, PHASES, DB_PATH,
# API_KEY, MEASURE_MEMORY.

set -euo pipefail
cd "$(dirname "$0")/.."

PORT="${PORT:-8080}"
WORKERS="${WORKERS:-4}"
THREADS="${THREADS:-8}"
DURATION="${DURATION:-15s}"
CONNS="${CONNS:-100 1000 5000}"
PHASES="${PHASES:-ping churn read write}"
DB_PATH="${DB_PATH:-stress_todos.db}"
MEASURE_MEMORY="${MEASURE_MEMORY:-1}"
export API_KEY="${API_KEY:-my-secret-api-key}"
BASE="http://127.0.0.1:${PORT}"

command -v wrk >/dev/null 2>&1 || {
    echo "wrk not found - install it first (e.g. 'brew install wrk' or 'apt install wrk')" >&2
    exit 1
}

TIME_CMD=()
if [ "$MEASURE_MEMORY" = "1" ] && [ -x /usr/bin/time ]; then
    if [ "$(uname -s)" = "Darwin" ]; then
        TIME_CMD=(/usr/bin/time -l)
    else
        TIME_CMD=(/usr/bin/time -v)
    fi
fi

SERVER_LOG="$(mktemp -t cexpress_server.XXXXXX)"
PEAK_FILE="$(mktemp -t cexpress_peak.XXXXXX)"

echo "==> Building release binary"
make -s

rm -f "$DB_PATH" "${DB_PATH}-shm" "${DB_PATH}-wal"

echo "==> Starting server (WORKERS=$WORKERS, QUIET=1, TODO_DB_PATH=$DB_PATH, memory tracking: $([ ${#TIME_CMD[@]} -gt 0 ] && echo "${TIME_CMD[*]}" || echo off))"
# stderr (server warnings + the time(1) report) goes to $SERVER_LOG, printed at the end.
QUIET=1 WORKERS="$WORKERS" PORT="$PORT" TODO_DB_PATH="$DB_PATH" API_KEY="$API_KEY" \
    ${TIME_CMD[@]+"${TIME_CMD[@]}"} ./build/bin/cexpress 2>"$SERVER_LOG" &
SERVER_PID=$!

# Under time(1), $SERVER_PID is time itself - the real server master is its child.
MASTER_PID=$SERVER_PID
if [ ${#TIME_CMD[@]} -gt 0 ]; then
    for _ in $(seq 1 50); do
        child="$(pgrep -P "$SERVER_PID" | head -n 1 || true)"
        if [ -n "$child" ]; then
            MASTER_PID=$child
            break
        fi
        sleep 0.1
    done
fi

STOPPED=0
stop_server() {
    if [ "$STOPPED" = "1" ]; then
        return
    fi
    STOPPED=1
    kill -TERM "${MASTER_PID:-}" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
}

cleanup() {
    stop_server
    rm -f "$DB_PATH" "${DB_PATH}-shm" "${DB_PATH}-wal" "$SERVER_LOG" "$PEAK_FILE"
}
trap cleanup EXIT

# Total RSS in KB of the master plus every live worker, and how many processes that was.
server_rss() {
    local pids
    pids="$MASTER_PID"
    local kids
    kids="$(pgrep -P "$MASTER_PID" 2>/dev/null | paste -sd, - || true)"
    if [ -n "$kids" ]; then
        pids="$pids,$kids"
    fi
    ps -o rss= -p "$pids" 2>/dev/null | awk '{ s += $1; n++ } END { print s+0, n+0 }' || true
}

# Largest single process RSS in KB (comparable to time -l's "maximum resident set size").
server_max_single_rss() {
    local pids
    pids="$MASTER_PID"
    local kids
    kids="$(pgrep -P "$MASTER_PID" 2>/dev/null | paste -sd, - || true)"
    if [ -n "$kids" ]; then
        pids="$pids,$kids"
    fi
    ps -o rss= -p "$pids" 2>/dev/null | awk '{ if ($1 > m) m = $1 } END { print m+0 }' || true
}

kb_to_mb() {
    awk -v kb="$1" 'BEGIN { printf "%.1f", kb / 1024 }'
}

echo "==> Waiting for server to come up"
for _ in $(seq 1 50); do
    curl -s -o /dev/null "$BASE/" && break
    sleep 0.2
done

echo "==> Seeding 20 todos"
for i in $(seq 1 20); do
    curl -s -o /dev/null -X POST "$BASE/api/todos" \
        -H "Authorization: Bearer $API_KEY" -H "Content-Type: application/json" \
        -d "{\"title\":\"seed todo $i\"}"
done

if [ "$MEASURE_MEMORY" = "1" ]; then
    read -r idle_total idle_procs <<<"$(server_rss)"
    echo "==> Idle memory after startup + seeding: $(kb_to_mb "$idle_total") MB total across $idle_procs processes"
fi

PEAK_SUMMARY=""

run_bench() {
    local label="$1"
    shift
    echo
    echo "=== $label ==="

    local sampler=""
    if [ "$MEASURE_MEMORY" = "1" ]; then
        echo "0 0 0" >"$PEAK_FILE"
        (
            peak_total=0
            peak_single=0
            peak_procs=0
            while true; do
                read -r total procs <<<"$(server_rss)"
                single="$(server_max_single_rss)"
                if [ "$total" -gt "$peak_total" ]; then
                    peak_total=$total
                    peak_procs=$procs
                fi
                if [ "$single" -gt "$peak_single" ]; then
                    peak_single=$single
                fi
                echo "$peak_total $peak_procs $peak_single" >"$PEAK_FILE"
                sleep 0.25
            done
        ) &
        sampler=$!
    fi

    wrk -t"$THREADS" -d"$DURATION" "$@"

    if [ -n "$sampler" ]; then
        kill "$sampler" 2>/dev/null || true
        wait "$sampler" 2>/dev/null || true
        read -r peak_total peak_procs peak_single <"$PEAK_FILE"
        echo "Peak server memory: $(kb_to_mb "$peak_total") MB total across $peak_procs processes (largest single process: $(kb_to_mb "$peak_single") MB)"
        PEAK_SUMMARY+="$(printf '%-52s %8s MB total   %8s MB largest process' "$label" "$(kb_to_mb "$peak_total")" "$(kb_to_mb "$peak_single")")"$'\n'
    fi
}

phase_enabled() {
    case " $PHASES " in *" $1 "*) return 0 ;; *) return 1 ;; esac
}

# /ping runs first: it never touches the table, so it cannot disturb the read numbers.
# Reads run next, against the small 20-row seeded table, at every concurrency level -
# this keeps read numbers comparable across tiers and well under TODO_LIST_MAX
# (app/todo_types.h), so db_list_todos never truncates. Writes run last, in their own
# pass: each POST benchmark grows the table further, which would otherwise silently
# make later read benchmarks measure against a bigger (eventually truncated) dataset
# if the phases were interleaved.
if phase_enabled ping; then
    for c in $CONNS; do
        run_bench "GET /ping (keep-alive)  threads=$THREADS conns=$c" -c"$c" "$BASE/ping"
    done
fi

if phase_enabled churn; then
    for c in $CONNS; do
        run_bench "GET /ping (conn: close) threads=$THREADS conns=$c" -c"$c" -H "Connection: close" "$BASE/ping"
    done
fi

if phase_enabled read; then
    for c in $CONNS; do
        run_bench "GET / (Todo UI)         threads=$THREADS conns=$c" -c"$c" "$BASE/"
        run_bench "GET /api/todos (read)   threads=$THREADS conns=$c" -c"$c" "$BASE/api/todos"
    done
fi

if phase_enabled write; then
    for c in $CONNS; do
        run_bench "POST /api/todos (write) threads=$THREADS conns=$c" -c"$c" -s scripts/wrk_create_todo.lua "$BASE/api/todos"
    done
fi

if [ "$MEASURE_MEMORY" = "1" ]; then
    read -r end_total end_procs <<<"$(server_rss)"
fi

echo
echo "==> Done. Stopping server"
stop_server

if [ "$MEASURE_MEMORY" = "1" ]; then
    echo
    echo "================ Memory summary ================"
    echo "Idle (after startup + seeding):  $(kb_to_mb "$idle_total") MB total across $idle_procs processes"
    echo "After all benchmarks (pre-stop): $(kb_to_mb "$end_total") MB total across $end_procs processes"
    echo
    echo "Peak per benchmark (sum of master + workers, sampled every 250ms):"
    printf '%s' "$PEAK_SUMMARY"
    echo
    echo "Note: summed RSS counts pages shared between processes (the binary, libc,"
    echo "SQLite, copy-on-write pages from the fork) once per process, so the total"
    echo "overstates real physical memory use. The largest-single-process figure is"
    echo "the more conservative per-worker number."
    if [ ${#TIME_CMD[@]} -gt 0 ]; then
        echo
        echo "================ ${TIME_CMD[*]} (whole server run) ================"
        echo "In cluster mode, 'maximum resident set size' (bytes) is the largest single"
        echo "process, and user/sys CPU time is summed across the master and every reaped"
        echo "worker. 'instructions retired', 'cycles elapsed' and 'peak memory footprint'"
        echo "cover the master process only - do not quote them as whole-server figures."
        cat "$SERVER_LOG"
    fi
fi

crashes="$(grep -c 'terminated by signal' "$SERVER_LOG" || true)"
echo
echo "Worker crashes during run: $crashes"
