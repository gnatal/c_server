#!/usr/bin/env bash
#
# Stress-tests the Todo CRUD demo (examples/todo_sqlite/) under multi-worker cluster mode
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
#          stresses accept/close. Every closed connection holds its client port in TIME_WAIT
#          for 2 x MSL (30 s on macOS), and the client has only ~16k ephemeral ports
#          (49152-65535 on macOS): at ~20k connections/s they are all gone in about a second
#          (MEASURED: 32,604 TIME_WAIT sockets after a 15 s run, then "connect" errors or wrk's
#          "Can't assign requested address"). So churn runs for CHURN_DURATION (default 2s,
#          not DURATION), and the script waits for TIME_WAIT to drain (below TIME_WAIT_MAX,
#          at most TIME_WAIT_WAIT seconds) before each churn run and before the next phase.
#   read   GET / and GET /api/todos (SQLite read path)
#   write  POST /api/todos (SQLite write path)
#
# Usage: scripts/stress_test.sh
#        PHASES=ping scripts/stress_test.sh          # connection stress only
#        PHASES="ping churn" CONNS="1000 5000" DURATION=10s scripts/stress_test.sh
# Tunable via env vars: PORT, WORKERS, THREADS, DURATION, CHURN_DURATION, CONNS, PHASES,
# DB_PATH, API_KEY, MEASURE_MEMORY, TIME_WAIT_MAX, TIME_WAIT_WAIT.
# A wrk run that fails (exit status != 0) is reported and the remaining runs still happen;
# the script then exits 1 after the summary.

set -euo pipefail
cd "$(dirname "$0")/.."

DEMO_DIR="examples/todo_sqlite"

PORT="${PORT:-8080}"
WORKERS="${WORKERS:-4}"
THREADS="${THREADS:-8}"
DURATION="${DURATION:-15s}"
CHURN_DURATION="${CHURN_DURATION:-2s}"
TIME_WAIT_MAX="${TIME_WAIT_MAX:-1000}"  # churn waits until fewer TIME_WAIT sockets than this remain
TIME_WAIT_WAIT="${TIME_WAIT_WAIT:-45}"  # ... but at most this many seconds (2 x MSL is 30 s on macOS)
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

# The engine sets SO_REUSEPORT, so a second server on the same port binds without error and the two
# silently share its connections - a leftover demo or stress run would answer part of this run.
if curl -s -o /dev/null --max-time 1 "$BASE/" || lsof -nP -iTCP:"$PORT" -sTCP:LISTEN >/dev/null 2>&1; then
    echo "error: something is already listening on port $PORT (see: lsof -nP -iTCP:$PORT -sTCP:LISTEN)." >&2
    echo "       Stop it or set PORT=..., otherwise it shares this run's connections (SO_REUSEPORT)." >&2
    exit 1
fi

SERVER_LOG="$(mktemp -t cexpress_server.XXXXXX)"
PEAK_FILE="$(mktemp -t cexpress_peak.XXXXXX)"

echo "==> Building release binary"
make demo -s

rm -f "$DEMO_DIR/$DB_PATH" "$DEMO_DIR/${DB_PATH}-shm" "$DEMO_DIR/${DB_PATH}-wal"

echo "==> Starting server (WORKERS=$WORKERS, QUIET=1, TODO_DB_PATH=$DB_PATH, memory tracking: $([ ${#TIME_CMD[@]} -gt 0 ] && echo "${TIME_CMD[*]}" || echo off))"
# Started from DEMO_DIR (not the repo root): the demo resolves public/ and its default
# TODO_DB_PATH relative to its own working directory, so from the repo root app_serve_static
# finds no public/ to register and GET / (and the whole "read" phase's UI check) 404s.
# `exec` inside the subshell replaces it with the server (or time(1)) directly, so SERVER_PID
# below is still that process's PID, not an intermediate shell - same assumption the
# time(1)-vs-master PID resolution right after this relies on.
# stderr (server warnings + the time(1) report) goes to $SERVER_LOG, printed at the end.
(
    cd "$DEMO_DIR"
    QUIET=1 WORKERS="$WORKERS" PORT="$PORT" TODO_DB_PATH="$DB_PATH" API_KEY="$API_KEY" \
        exec ${TIME_CMD[@]+"${TIME_CMD[@]}"} ./cexpress_demo
) 2>"$SERVER_LOG" &
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
    local status=$?
    stop_server
    if [ "$status" -ne 0 ] && [ -s "${SERVER_LOG:-}" ]; then
        echo "==> Server log (stderr):" >&2
        tail -n 40 "$SERVER_LOG" >&2
    fi
    rm -f "$DEMO_DIR/$DB_PATH" "$DEMO_DIR/${DB_PATH}-shm" "$DEMO_DIR/${DB_PATH}-wal" "$SERVER_LOG" "$PEAK_FILE"
}
trap cleanup EXIT
# set -e exits silently; name the failing line and command first.
trap 'echo "error: stress_test.sh line $LINENO failed (exit $?): $BASH_COMMAND" >&2' ERR

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
ready=0
for _ in $(seq 1 50); do
    if curl -s -o /dev/null "$BASE/"; then
        ready=1
        break
    fi
    sleep 0.2
done
if [ "$ready" != "1" ]; then
    echo "error: server did not answer GET / on port $PORT within 10 s" >&2
    exit 1
fi

echo "==> Seeding 20 todos"
for i in $(seq 1 20); do
    rc=0
    code="$(curl -sS -o /dev/null -w '%{http_code}' -X POST "$BASE/api/todos" \
        -H "Authorization: Bearer $API_KEY" -H "Content-Type: application/json" \
        -d "{\"title\":\"seed todo $i\"}")" || rc=$?
    if [ "$rc" -ne 0 ] || [ "$code" != "201" ]; then
        echo "error: seed request $i failed: curl exit $rc, HTTP status ${code:-none}" >&2
        exit 1
    fi
done

if [ "$MEASURE_MEMORY" = "1" ]; then
    read -r idle_total idle_procs <<<"$(server_rss)"
    echo "==> Idle memory after startup + seeding: $(kb_to_mb "$idle_total") MB total across $idle_procs processes"
fi

PEAK_SUMMARY=""
FAILED_RUNS=""

# TCP sockets in TIME_WAIT on this host (both ends of a loopback connection count).
time_wait_count() {
    if command -v ss >/dev/null 2>&1; then
        ss -tan state time-wait 2>/dev/null | tail -n +2 | wc -l | tr -d ' '
    else
        netstat -an -p tcp 2>/dev/null | grep -c TIME_WAIT || true
    fi
}

# Waits until TIME_WAIT sockets drop below TIME_WAIT_MAX (at most TIME_WAIT_WAIT seconds), so a churn run
# starts with free client ports and the next phase does not inherit exhausted ones.
wait_for_time_wait_drain() {
    local count waited=0
    count="$(time_wait_count)"
    if [ "$count" -lt "$TIME_WAIT_MAX" ]; then
        return
    fi
    echo "==> Waiting for TIME_WAIT sockets to drain ($count, want < $TIME_WAIT_MAX, up to ${TIME_WAIT_WAIT}s)"
    while [ "$count" -ge "$TIME_WAIT_MAX" ] && [ "$waited" -lt "$TIME_WAIT_WAIT" ]; do
        sleep 1
        waited=$((waited + 1))
        count="$(time_wait_count)"
    done
    echo "==> TIME_WAIT sockets: $count after ${waited}s"
}

# run_bench LABEL DURATION WRK_ARGS...
run_bench() {
    local label="$1"
    local duration="$2"
    shift 2
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

    local wrk_status=0
    wrk -t"$THREADS" -d"$duration" "$@" || wrk_status=$?
    if [ "$wrk_status" -ne 0 ]; then
        echo "warning: wrk exited with status $wrk_status - continuing with the next run" >&2
        FAILED_RUNS+="  $label (wrk exit $wrk_status)"$'\n'
    fi

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
# (examples/todo_sqlite/todo_types.h), so db_list_todos never truncates. Writes run last, in their own
# pass: each POST benchmark grows the table further, which would otherwise silently
# make later read benchmarks measure against a bigger (eventually truncated) dataset
# if the phases were interleaved.
if phase_enabled ping; then
    for c in $CONNS; do
        run_bench "GET /ping (keep-alive)  threads=$THREADS conns=$c" "$DURATION" -c"$c" "$BASE/ping"
    done
fi

if phase_enabled churn; then
    for c in $CONNS; do
        wait_for_time_wait_drain
        run_bench "GET /ping (conn: close) threads=$THREADS conns=$c" "$CHURN_DURATION" -c"$c" -H "Connection: close" "$BASE/ping"
    done
    wait_for_time_wait_drain # the next phase needs client ports too
fi

if phase_enabled read; then
    for c in $CONNS; do
        run_bench "GET / (Todo UI)         threads=$THREADS conns=$c" "$DURATION" -c"$c" "$BASE/"
        run_bench "GET /api/todos (read)   threads=$THREADS conns=$c" "$DURATION" -c"$c" "$BASE/api/todos"
    done
fi

if phase_enabled write; then
    for c in $CONNS; do
        run_bench "POST /api/todos (write) threads=$THREADS conns=$c" "$DURATION" -c"$c" -s scripts/wrk_create_todo.lua "$BASE/api/todos"
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
if [ -n "$FAILED_RUNS" ]; then
    echo
    echo "Failed wrk runs (see their warnings above):"
    printf '%s' "$FAILED_RUNS"
    exit 1
fi
