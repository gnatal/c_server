#!/usr/bin/env bash
#
# Stress-tests the Todo CRUD demo (examples/todo_sqlite/) running inside Docker, in cluster mode,
# using wrk - the containerized counterpart to stress_test.sh, exercising the Linux backends from a
# Mac (epoll by default; CEXPRESS_EVENT_LOOP=io_uring is passed through to the server to measure io_uring). wrk runs as its own container on the same private Docker network as the server and
# talks to it container-to-container, bypassing Docker Desktop's localhost proxy (vpnkit) entirely
# for the actual measurement traffic (only the readiness wait and seeding below go through the
# host's port mapping).
#
# Same phase/output/memory-tracking pattern as stress_test.sh - see that script and scripts/CLAUDE.md
# for the shared rationale and phase semantics. Two deliberate differences:
#   - wrk itself runs from a locally-built image (scripts/wrk.Dockerfile), not a pulled one: on an
#     Apple Silicon host, a pulled amd64-only wrk image runs under QEMU emulation, making the *load
#     generator* the bottleneck and understating the server's real throughput - exactly the kind of
#     measurement error running inside Docker for accurate Linux-to-Linux numbers is trying to avoid.
#     Building from source targets whatever platform `docker build` runs on (matching this repo's own
#     vendored-dependency convention, see lib/vendor/); Docker's layer cache keeps repeat runs fast.
#   - No /usr/bin/time -l equivalent: that's a host rusage tool with no clean containerized analogue.
#     Peak memory here comes entirely from container_mem_stats below (a /proc-based sampler run via
#     `docker exec`, summing VmRSS across every `cexpress_demo` process inside the server container -
#     master plus every worker; PID 1 is always the master, since the Dockerfile's CMD is exec-form
#     with no shell wrapper). It samples every 500ms rather than stress_test.sh's 250ms: each sample
#     is a real process spawn through containerd/runc, not a local `ps` call, so a tighter interval
#     would add enough sampling overhead to perturb the benchmark it's trying to measure.
#
# Phases (PHASES, space-separated, default "ping churn read write"): identical semantics to
# stress_test.sh, against the containerized server instead.
#   ping   GET /ping over keep-alive connections: pure engine throughput, no DB, no JSON
#   churn  GET /ping with "Connection: close": a new TCP connection per request
#   read   GET / and GET /api/todos (SQLite read path)
#   write  POST /api/todos (SQLite write path, via scripts/wrk_create_todo.lua)
#
# Usage: scripts/docker_stress_test.sh
#        PHASES=ping scripts/docker_stress_test.sh          # connection stress only
#        PHASES="ping churn" CONNS="1000 5000" DURATION=15s scripts/docker_stress_test.sh
# Tunable via env vars: PORT, WORKERS, THREADS, DURATION, CONNS, PHASES, DB_PATH, API_KEY,
# MEASURE_MEMORY.

set -euo pipefail
cd "$(dirname "$0")/.."

PORT="${PORT:-8080}"
WORKERS="${WORKERS:-4}"
THREADS="${THREADS:-8}"
DURATION="${DURATION:-10s}"
CONNS="${CONNS:-100 1000}"
PHASES="${PHASES:-ping churn read write}"
DB_PATH="${DB_PATH:-stress_todos.db}"
MEASURE_MEMORY="${MEASURE_MEMORY:-1}"
export API_KEY="${API_KEY:-my-secret-api-key}"

NETWORK="cserver-bench-net"
SERVER_CONTAINER="c-server-bench"
IMAGE_NAME="c-server-demo"
WRK_IMAGE="cexpress-wrk-bench"
BASE="http://127.0.0.1:${PORT}"               # administrative access (readiness wait, seeding) via the host port mapping
TARGET="http://${SERVER_CONTAINER}:${PORT}"    # what wrk actually hits, container-to-container

command -v docker >/dev/null 2>&1 || {
    echo "docker not found - install Docker Desktop (or the docker CLI) first" >&2
    exit 1
}

SERVER_LOG="$(mktemp -t cexpress_docker_server.XXXXXX)"
PEAK_FILE="$(mktemp -t cexpress_docker_peak.XXXXXX)"

echo "==> Building Linux Docker image ($IMAGE_NAME)"
docker build -t "$IMAGE_NAME" -q . >/dev/null

echo "==> Building wrk image ($WRK_IMAGE, native arch - see scripts/wrk.Dockerfile)"
docker build -f scripts/wrk.Dockerfile -t "$WRK_IMAGE" -q scripts >/dev/null

echo "==> Creating Docker network ($NETWORK)"
docker network create "$NETWORK" >/dev/null 2>&1 || true

echo "==> Starting server (WORKERS=$WORKERS, QUIET=1, TODO_DB_PATH=$DB_PATH)"
docker rm -f "$SERVER_CONTAINER" >/dev/null 2>&1 || true
docker run -d \
    --name "$SERVER_CONTAINER" \
    --network "$NETWORK" \
    -p "$PORT:$PORT" \
    --security-opt seccomp=unconfined \
    --ulimit memlock=-1:-1 \
    -e WORKERS="$WORKERS" \
    -e QUIET=1 \
    -e PORT="$PORT" \
    -e TODO_DB_PATH="$DB_PATH" \
    -e API_KEY="$API_KEY" \
    -e CEXPRESS_EVENT_LOOP="${CEXPRESS_EVENT_LOOP:-}" \
    "$IMAGE_NAME" >/dev/null

STOPPED=0
stop_server() {
    if [ "$STOPPED" = "1" ]; then
        return
    fi
    STOPPED=1
    # docker logs before stopping: the container (and its stdout/stderr) is gone once removed below.
    docker logs "$SERVER_CONTAINER" >"$SERVER_LOG" 2>&1 || true
    docker stop "$SERVER_CONTAINER" >/dev/null 2>&1 || true
}

cleanup() {
    stop_server
    docker rm -f "$SERVER_CONTAINER" >/dev/null 2>&1 || true
    docker network rm "$NETWORK" >/dev/null 2>&1 || true
    rm -f "$SERVER_LOG" "$PEAK_FILE"
}
trap cleanup EXIT

# Sums /proc/<pid>/status VmRSS (KB) for every `cexpress_demo` process inside the server container
# (master + every worker) in one `docker exec` - "total procs single", the same two numbers
# stress_test.sh's server_rss/server_max_single_rss report, combined into one call so a sample only
# pays docker exec's overhead once instead of twice.
container_mem_stats() {
    docker exec "$SERVER_CONTAINER" sh -c '
        total=0; n=0; max=0
        for p in /proc/[0-9]*; do
            [ -r "$p/comm" ] || continue
            comm=$(cat "$p/comm" 2>/dev/null) || continue
            [ "$comm" = "cexpress_demo" ] || continue
            rss=$(awk "/VmRSS/{print \$2}" "$p/status" 2>/dev/null)
            [ -n "$rss" ] || continue
            total=$((total+rss)); n=$((n+1))
            [ "$rss" -gt "$max" ] && max=$rss
        done
        echo "$total $n $max"
    ' 2>/dev/null || echo "0 0 0"
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
    read -r idle_total idle_procs _ <<<"$(container_mem_stats)"
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
                read -r total procs single <<<"$(container_mem_stats)"
                if [ "$total" -gt "$peak_total" ]; then
                    peak_total=$total
                    peak_procs=$procs
                fi
                if [ "$single" -gt "$peak_single" ]; then
                    peak_single=$single
                fi
                echo "$peak_total $peak_procs $peak_single" >"$PEAK_FILE"
                sleep 0.5
            done
        ) &
        sampler=$!
    fi

    docker run --rm --network "$NETWORK" -v "$(pwd)/scripts:/scripts:ro" -e API_KEY="$API_KEY" \
        "$WRK_IMAGE" -t"$THREADS" -d"$DURATION" "$@"

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

# Same order and reasoning as stress_test.sh: /ping first (never touches the table), reads next
# against the small 20-row seeded table at every concurrency level, writes last in their own pass
# (each POST benchmark grows the table, which would otherwise make later read benchmarks measure
# against a bigger - eventually truncated - dataset if the phases were interleaved).
if phase_enabled ping; then
    for c in $CONNS; do
        run_bench "GET /ping (keep-alive)  threads=$THREADS conns=$c" -c"$c" "$TARGET/ping"
    done
fi

if phase_enabled churn; then
    for c in $CONNS; do
        run_bench "GET /ping (conn: close) threads=$THREADS conns=$c" -c"$c" -H "Connection: close" "$TARGET/ping"
    done
fi

if phase_enabled read; then
    for c in $CONNS; do
        run_bench "GET / (Todo UI)         threads=$THREADS conns=$c" -c"$c" "$TARGET/"
        run_bench "GET /api/todos (read)   threads=$THREADS conns=$c" -c"$c" "$TARGET/api/todos"
    done
fi

if phase_enabled write; then
    for c in $CONNS; do
        run_bench "POST /api/todos (write) threads=$THREADS conns=$c" -c"$c" -s /scripts/wrk_create_todo.lua "$TARGET/api/todos"
    done
fi

if [ "$MEASURE_MEMORY" = "1" ]; then
    read -r end_total end_procs _ <<<"$(container_mem_stats)"
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
    echo "Peak per benchmark (sum of master + workers inside the container, sampled every 500ms):"
    printf '%s' "$PEAK_SUMMARY"
    echo
    echo "Note: summed RSS counts pages shared between processes (the binary, libc, SQLite, fork"
    echo "copy-on-write pages) once per process, so the total overstates real physical memory use."
    echo "The largest-single-process figure is the more conservative per-worker number. No"
    echo "/usr/bin/time -l equivalent here (see script header) - this is the only memory source."
fi

crashes="$(grep -c 'terminated by signal' "$SERVER_LOG" || true)"
echo
echo "Worker crashes during run: $crashes"
