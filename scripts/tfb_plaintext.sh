#!/usr/bin/env bash
#
# A local, small-scale copy of the TechEmpower plaintext test: the TFB entry's /plaintext handler
# (scripts/tfb_plaintext.c), TFB's own wrk command (same headers, --latency, --timeout 8, TFB's
# pipeline.lua at depth 16), a primer and a warmup run, then one run per concurrency level.
# Server and wrk share this machine, so absolute numbers are lower than TFB's; compare against a
# baseline built the same way, in the same run.
#
# Usage: scripts/tfb_plaintext.sh                 # the working tree only
#        scripts/tfb_plaintext.sh HEAD            # working tree vs. a git ref (built in a temp worktree)
#        LEVELS="256 1024" DURATION=5 scripts/tfb_plaintext.sh HEAD
#
# Env (defaults): LEVELS ("256 1024 4096"; TFB also runs 16384, which needs more than the ~16k
# ephemeral ports macOS gives one loopback destination), DURATION (15, TFB's), WARMUP (5),
# WORKERS (1; 0 = one per core, like the TFB image), THREADS (4, wrk threads), PIPELINE (16), PORT (18080).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BASELINE_REF="${1:-}"
LEVELS="${LEVELS:-256 1024 4096}"
DURATION="${DURATION:-15}"
WARMUP="${WARMUP:-5}"
WORKERS="${WORKERS:-1}"
THREADS="${THREADS:-4}"
PIPELINE="${PIPELINE:-16}"
PORT="${PORT:-18080}"
CC="${CC:-$(command -v gcc-16 || command -v gcc)}"
URL="http://127.0.0.1:${PORT}/plaintext"

command -v wrk >/dev/null || { echo "wrk not found (brew install wrk)" >&2; exit 1; }

WORK="$(mktemp -d)"
SERVER_PID=""
cleanup() {
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null && wait "$SERVER_PID" 2>/dev/null || true
    if [ -d "$WORK/base" ]; then
        git -C "$ROOT" worktree remove --force "$WORK/base" >/dev/null 2>&1 || true
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT

# TFB's pipeline.lua (toolset/wrk/pipeline.lua): `depth` copies of the request in one write.
cat > "$WORK/pipeline.lua" <<'EOF'
init = function(args)
  local r = {}
  local depth = tonumber(args[1]) or 1
  for i=1,depth do
    r[i] = wrk.format()
  end
  req = table.concat(r)
end

request = function()
  return req
end
EOF

# build <source tree> <output binary>
build() {
    make -C "$1" all >/dev/null
    "$CC" -O2 -I"$1/lib" "$ROOT/scripts/tfb_plaintext.c" "$1/build/lib/libcexpress.a" -o "$2"
}

# TFB's wrk invocation for plaintext (toolset/benchmark/framework_test.py: headers, --latency, --timeout 8).
tfb_wrk() { # <connections> <seconds> [pipeline depth]
    local c="$1" d="$2" depth="${3:-}" t="$THREADS"
    [ "$c" -lt "$t" ] && t="$c"
    wrk -H 'Host: localhost' \
        -H 'Accept: text/plain,text/html;q=0.9,application/xhtml+xml;q=0.9,application/xml;q=0.8,*/*;q=0.7' \
        -H 'Connection: keep-alive' --latency -d "$d" -c "$c" --timeout 8 -t "$t" "$URL" \
        ${depth:+-s "$WORK/pipeline.lua" -- "$depth"}
}

# run <label> <binary>: primer, warmup, then each level; appends "label level req/s errors" to $WORK/results
run() {
    local label="$1" bin="$2"
    PORT="$PORT" CEXPRESS_WORKERS="$WORKERS" "$bin" >/dev/null 2>&1 &
    SERVER_PID=$!
    for _ in $(seq 50); do curl -s -o /dev/null "$URL" && break; sleep 0.1; done
    curl -s -o /dev/null "$URL" || { echo "$label: server did not start" >&2; exit 1; }

    local max_level="${LEVELS##* }"
    echo "[$label] primer (8 connections, 5 s) and warmup ($max_level connections, ${WARMUP} s)"
    tfb_wrk 8 5 >/dev/null
    tfb_wrk "$max_level" "$WARMUP" "$PIPELINE" >/dev/null

    for c in $LEVELS; do
        local out rps errors
        out="$(tfb_wrk "$c" "$DURATION" "$PIPELINE" 2>&1)"
        rps="$(awk '/Requests\/sec/ {print $2}' <<<"$out")"
        errors="$(awk '/Socket errors|Non-2xx/ {sub(/^ +/, ""); printf "%s; ", $0}' <<<"$out")"
        printf '[%s] %6s connections: %12s req/s  %s\n' "$label" "$c" "$rps" "${errors:-no errors}"
        echo "$label $c $rps" >> "$WORK/results"
    done

    kill "$SERVER_PID"; wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""
}

echo "building working tree"
build "$ROOT" "$WORK/current"
if [ -n "$BASELINE_REF" ]; then
    echo "building $BASELINE_REF"
    git -C "$ROOT" worktree add -q --detach "$WORK/base" "$BASELINE_REF"
    build "$WORK/base" "$WORK/baseline"
fi

echo "plaintext: pipeline $PIPELINE, ${DURATION} s per level, workers $WORKERS, wrk threads $THREADS"
[ -n "$BASELINE_REF" ] && run "$BASELINE_REF" "$WORK/baseline"
run "current" "$WORK/current"

if [ -n "$BASELINE_REF" ]; then
    echo
    printf '%-12s %14s %14s %8s\n' connections "$BASELINE_REF" current ratio
    for c in $LEVELS; do
        base="$(awk -v r="$BASELINE_REF" -v c="$c" '$1 == r && $2 == c {print $3}' "$WORK/results")"
        cur="$(awk -v c="$c" '$1 == "current" && $2 == c {print $3}' "$WORK/results")"
        printf '%-12s %14s %14s %7.2fx\n' "$c" "$base" "$cur" "$(awk -v a="$cur" -v b="$base" 'BEGIN {print (b > 0 ? a / b : 0)}')"
    done
fi
