#!/usr/bin/env bash
#
# docker_stress_test.sh
# 
# Stress-tests the Todo CRUD demo running inside Docker (to utilize io_uring)
# using a dockerized version of `wrk`. 
# By running `wrk` inside the same Docker network as the server, we bypass 
# Docker Desktop's localhost network proxy (vpnkit) and measure the true 
# Linux-to-Linux io_uring throughput.

set -euo pipefail
cd "$(dirname "$0")/.."

PORT="${PORT:-8080}"
THREADS="${THREADS:-8}"
DURATION="${DURATION:-10s}"
CONNS="${CONNS:-100 1000}"
PHASES="${PHASES:-ping churn read write}"
export API_KEY="${API_KEY:-my-secret-api-key}"

NETWORK="cserver-bench-net"
SERVER_CONTAINER="c-server-bench"
IMAGE_NAME="c-server-demo"

echo "==> Building Linux Docker image ($IMAGE_NAME)..."
docker build -t "$IMAGE_NAME" -q .

echo "==> Creating Docker network ($NETWORK)..."
docker network create "$NETWORK" >/dev/null 2>&1 || true

echo "==> Starting c_server_demo with io_uring enabled..."
docker rm -f "$SERVER_CONTAINER" >/dev/null 2>&1 || true

# We map the port to localhost so we can easily seed it via curl, 
# but the benchmarks will talk directly container-to-container on the network.
docker run -d \
    --name "$SERVER_CONTAINER" \
    --network "$NETWORK" \
    -p "$PORT:$PORT" \
    --security-opt seccomp=unconfined \
    --ulimit memlock=-1:-1 \
    -e WORKERS=4 \
    -e QUIET=1 \
    "$IMAGE_NAME" > /dev/null

# Ensure cleanup happens on exit (Ctrl+C or script finish)
cleanup() {
    echo "==> Shutting down server and cleaning up..."
    docker stop "$SERVER_CONTAINER" >/dev/null 2>&1 || true
    docker rm -f "$SERVER_CONTAINER" >/dev/null 2>&1 || true
    docker network rm "$NETWORK" >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "==> Waiting for server to come up..."
for _ in $(seq 1 50); do
    curl -s -o /dev/null "http://127.0.0.1:${PORT}/" && break
    sleep 0.2
done

echo "==> Seeding 20 todos..."
for i in $(seq 1 20); do
    curl -s -o /dev/null -X POST "http://127.0.0.1:${PORT}/api/todos" \
        -H "Authorization: Bearer $API_KEY" -H "Content-Type: application/json" \
        -d "{\"title\":\"seed todo $i\"}"
done

# Create the LUA script for the write test as a temporary file in the workspace
cat > /tmp/post.lua <<EOF
wrk.method = "POST"
wrk.body = '{"title":"stress"}'
wrk.headers["Authorization"] = "Bearer $API_KEY"
wrk.headers["Content-Type"] = "application/json"
EOF

run_bench() {
    local label="$1"
    shift
    echo
    echo "=== $label ==="
    for conn in $CONNS; do
        echo "--> Concurrency: $conn"
        docker run --rm --network "$NETWORK" alpine/bombardier -c "$conn" -d "$DURATION" "$@"
    done
}

for phase in $PHASES; do
    case "$phase" in
    ping)
        run_bench "Ping (Keep-Alive)" "http://$SERVER_CONTAINER:$PORT/ping"
        ;;
    churn)
        # bombardier does not have a direct Connection: close flag like wrk. 
        # But we can use HTTP/1.0 or send a custom header.
        run_bench "Ping (Connection: close)" -H "Connection: close" "http://$SERVER_CONTAINER:$PORT/ping"
        ;;
    read)
        run_bench "Root (Static / HTML)" "http://$SERVER_CONTAINER:$PORT/"
        run_bench "API GET /todos (SQLite Read)" "http://$SERVER_CONTAINER:$PORT/api/todos"
        ;;
    write)
        echo
        echo "=== API POST /todos (SQLite Write) ==="
        for conn in $CONNS; do
            echo "--> Concurrency: $conn"
            docker run --rm --network "$NETWORK" alpine/bombardier -c "$conn" -d "$DURATION" \
                -m POST -H "Authorization: Bearer $API_KEY" -H "Content-Type: application/json" \
                -b '{"title":"stress"}' \
                "http://$SERVER_CONTAINER:$PORT/api/todos"
        done
        ;;
    *)
        echo "Unknown phase: $phase" >&2
        ;;
    esac
done
