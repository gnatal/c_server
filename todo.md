# Running the Todo CRUD demo

## Build & run (local)

```bash
make            # builds build/bin/cexpress
make run        # or: ./build/bin/cexpress
make test       # runs the full lib/ test suite
make clean      # wipe build/ output
```

Server listens on `http://localhost:8080` by default. Useful env vars:

```bash
PORT=3000 API_KEY=my-secret-api-key TODO_DB_PATH=todos.db ./build/bin/cexpress
WORKERS=4 ./build/bin/cexpress   # multi-process cluster mode
QUIET=1 ./build/bin/cexpress     # disable per-request access logging
```

Open `http://localhost:8080/` in a browser for the built-in Todo UI.

## Build & run (Docker)

```bash
docker build -t cexpress-todo .
docker run --rm -p 8080:8080 cexpress-todo

# with options:
docker run --rm -p 8080:8080 -e WORKERS=4 -e API_KEY=my-secret-api-key cexpress-todo
```

## API key

Default: `my-secret-api-key` (override via `API_KEY` env var). Only required
for `POST`/`PUT`/`PATCH`/`DELETE` — `GET` routes are public.

## curl examples

```bash
BASE=http://localhost:8080
KEY=my-secret-api-key

# List all todos
curl -s $BASE/api/todos

# List only completed / only pending
curl -s "$BASE/api/todos?done=true"
curl -s "$BASE/api/todos?done=false"

# Get one by id
curl -s $BASE/api/todos/1

# Create
curl -s -X POST $BASE/api/todos \
  -H "Authorization: Bearer $KEY" \
  -H "Content-Type: application/json" \
  -d '{"title":"Buy milk"}'

# Replace (PUT - title + done both required)
curl -s -X PUT $BASE/api/todos/1 \
  -H "Authorization: Bearer $KEY" \
  -H "Content-Type: application/json" \
  -d '{"title":"Buy oat milk","done":false}'

# Partial update (PATCH - either field optional)
curl -s -X PATCH $BASE/api/todos/1 \
  -H "Authorization: Bearer $KEY" \
  -H "Content-Type: application/json" \
  -d '{"done":true}'

# Delete (204 No Content)
curl -s -X DELETE $BASE/api/todos/1 -H "Authorization: Bearer $KEY"
```

## Stress test / benchmark

Requires `wrk` (`brew install wrk`). One command builds, boots a 4-worker
cluster, seeds some todos, and runs `wrk` against `GET /`, `GET /api/todos`,
and `POST /api/todos` at 100/1000/5000 connections:

```bash
scripts/stress_test.sh
```

Tunable via env vars:

```bash
WORKERS=8 THREADS=8 DURATION=15s CONNS="100 1000 5000 10000" scripts/stress_test.sh
```

The script also tracks memory: it runs the server under `/usr/bin/time -l`,
samples the RSS of the master and every worker during each benchmark, and prints
a memory summary at the end. Add `MEASURE_MEMORY=0` to turn that off. Results of a
full run are in `stress_tests/stress_test_report.md`.
