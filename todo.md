# Running the Todo CRUD demo

(This file is a run cheat sheet for the demo, not a task list. Project docs: `README.md`, `DOC.md`, `lib/CLAUDE.md`.)

## Build & run (local)

```bash
make demo                       # from the repo root: builds build/lib/libcexpress.a and examples/todo_sqlite/cexpress_demo
cd examples/todo_sqlite
./cexpress_demo                 # or: make run
cd ../.. && make test           # runs the full test suite (14 suites)
make clean                      # wipe build/ and the demo's objects and binary
```

The demo loads `public/` and creates `todos.db` relative to the directory it is started from, so run it from
`examples/todo_sqlite/`. (After changing `lib/`, rebuild the demo with `make -B -C examples/todo_sqlite`; its
Makefile does not track the library.)

Server listens on `http://localhost:8080` by default. Useful env vars:

```bash
PORT=3000 API_KEY=my-secret-api-key TODO_DB_PATH=todos.db ./cexpress_demo
WORKERS=4 ./cexpress_demo   # multi-process cluster mode
QUIET=1 ./cexpress_demo     # disable per-request access logging
```

Open `http://localhost:8080/` in a browser for the built-in Todo UI.

## Build & run (Docker)

The image compiles the library and the demo on Alpine (io_uring event loop) and does not run the tests.

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

Requires `wrk` (`brew install wrk`). `scripts/stress_test.sh` builds the demo, boots a 4-worker cluster, seeds some
todos, and runs `wrk` against `/ping`, `GET /`, `GET /api/todos` and `POST /api/todos`:

```bash
scripts/stress_test.sh
```

Tunable via env vars:

```bash
WORKERS=8 THREADS=8 DURATION=15s CONNS="100 1000 5000 10000" scripts/stress_test.sh
```

Read `scripts/CLAUDE.md` first: as of 2026-09-21 the script starts the server from the repository root (so its
`GET /` rows measure a 404), and its memory sampler output is not credible. It tracks memory by running the server
under `/usr/bin/time -l` and sampling the RSS of the master and every worker; add `MEASURE_MEMORY=0` to turn that
off. Results and the manual `wrk` commands are in `stress_tests/stress_test_report.md`.
