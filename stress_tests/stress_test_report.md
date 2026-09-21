# CExpress Todo API — Stress Test Report

## Status of the data in this report

| Part | Date | Engine | Method | Use it for |
|---|---|---|---|---|
| **A. Current measurements** | 2026-09-21 | current (arena, yyjson, picohttpparser, Patricia router) | manual `wrk` commands and a small connection-holding script, server started from `examples/todo_sqlite/` | the numbers to quote today |
| **B. Earlier run** | 2026-09-18 | before those changes (handwritten parser, linear router, malloc'd buffers, in-house JSON) | `scripts/stress_test.sh` | history, and the two bugs it found |
| **C. `stress_test.sh` run** | 2026-09-21 | current | `scripts/stress_test.sh` → `full_run_output.txt` | nothing: two methodology problems, listed below |

Every figure is a single run on one Apple M3 Pro laptop (macOS, gcc-16 -O2) with `wrk` (8 threads, keep-alive, 15 s) on the same machine as the server.

---

## A. Current measurements (2026-09-21)

### Setup

| | |
|---|---|
| Server | `QUIET=1 WORKERS=4 ./cexpress_demo`, started from `examples/todo_sqlite/` (SQLite file in WAL mode, `busy_timeout=5000`, one private connection per worker) |
| Dataset | 20 seeded todos for the read rows; the write row ran last |
| Auth | writes send `Authorization: Bearer <API_KEY>` (`scripts/wrk_create_todo.lua`) |
| Not measured | Linux / io_uring; separate load-generator machine; repeated runs |

### Throughput

| Endpoint | Connections | Requests/sec | Avg latency | Max latency |
|---|---|---|---|---|
| `GET /ping` (4-byte reply, no DB, no JSON) | 100 | 250,055 | 393 µs | 19.07 ms |
| `GET /ping` | 1,000 | 207,903 | 4.81 ms | 19.54 ms |
| `GET /ping` | 5,000 | 238,859 | 14.72 ms | 175.88 ms |
| `GET /ping`, `WORKERS=1` | 100 | 221,611 | 359 µs | 6.20 ms |
| `GET /` (Todo UI, 6,481 B streamed from disk) | 100 | 60,466 | 1.60 ms | 19.16 ms |
| `GET /api/todos` (SQLite read, 20 rows) | 100 | 69,011 | 1.39 ms | 5.70 ms |
| `GET /api/todos` | 1,000 | 61,999 | 16.05 ms | 40.73 ms |
| `POST /api/todos` (SQLite write) | 100 | 23,846 | 4.13 ms | 42.85 ms |

Notes:
- With one worker `/ping` is within 12% of four workers, so these runs are bound by the shared machine (and `wrk`), not by worker count. See the distribution finding below.
- An earlier `GET /api/todos` attempt in the same session ran against a database that had accumulated about 120 rows across restarts and returned 21,016 req/s with 13 KB responses. It is not in the table, but it shows how much the row count matters: seed a fresh 20-row database before each read benchmark.
- Only `wrk`'s summary lines were recorded for these rows (total requests and transfer volume were not kept).

### Memory (`ps` RSS, four workers plus master)

A script opened N keep-alive connections to the 4-worker cluster and sent one `GET /ping` on each, then RSS was read per process:

| Open keep-alive connections | Total, 5 processes | Largest single process |
|---|---|---|
| 0 (idle after boot) | 11.5 MB | 2.8 MB |
| 100 | 17.1 MB | 7.7 MB |
| 1,000 | 38.5 MB | 29.1 MB |
| 5,000 | 133.7 MB | 124.3 MB |

A separate one-worker measurement (5,000 connections, idle or after one request each): about 121 MB, about **25 KB per connection**. That is the 8 KiB input buffer plus the touched part of the 64 KiB per-connection arena (`tradeoffs.md`); the earlier engine measured about 7 KB per connection (Part B). Summed RSS counts shared pages (binary, libc, SQLite, fork copy-on-write) once per process, so totals overstate physical memory.

### Findings

1. **Connections were not spread across workers on macOS.** With 5,000 connections from one client, the largest worker held 124.3 of 133.7 MB (about 93%), and at 100 connections 7.7 of 17.1 MB. `SO_REUSEPORT` does not appear to balance TCP connections between the four sockets here. Linux was not tested. See `concurrency.md`.
2. **`wrk` read errors at 5,000 connections persist**: 2,219 errors on `/ping` in this session (no connect errors). Part B and Part C recorded the same symptom, including 1,624 to 2,393 errors per run in the `stress_test.sh` run. The cause is still unidentified, and it may be the load generator or the operating system rather than the server.
3. **No worker crashed** in the final run's log (`POST`, 4 workers); logs from the earlier runs of the session were not kept.
4. **Malformed requests are not answered.** Not a stress-test result, but found while checking the server's behavior under bad input: a request line without a version, `HTTP/2.0` or plain garbage gets no reply and the connection stays open (`lib/CLAUDE.md`, "Known gaps").

---

## B. Earlier run (2026-09-18, previous engine)

Kept for history; **do not compare directly with Part A** (different engine, different method, and Part C shows the script's own numbers can be off).

Setup: same machine and tools; `stress_test.sh`; 4 workers; 20 seeded todos for the read benchmarks, writes last; connections 100 / 1,000 / 5,000.

| Endpoint | Connections | Requests/sec | Avg latency | Max latency |
|---|---|---|---|---|
| `GET /` (Todo UI, streamed from disk) | 100 | 58,336 | 1.64 ms | 7.6 ms |
| `GET /` | 1,000 | 57,953 | 17.2 ms | 53.1 ms |
| `GET /` | 5,000 | 54,503 | 61.4 ms | 153.1 ms |
| `GET /api/todos` (SQLite read) | 100 | 27,250 | 3.50 ms | 9.2 ms |
| `GET /api/todos` | 1,000 | 26,989 | 36.9 ms | 70.7 ms |
| `GET /api/todos` | 5,000 | 25,575 | 171.8 ms | 378.3 ms |
| `POST /api/todos` (SQLite write) | 100 | 22,183 | 5.02 ms | 158.0 ms |
| `POST /api/todos` | 1,000 | 22,162 | 44.9 ms | 86.3 ms |
| `POST /api/todos` | 5,000 | 21,615 | 131.0 ms | 351.4 ms |

Memory then: about 12 MB total idle, about 13 MB at 58k req/s over 100 connections, about 50 MB total (45 MB in the largest process) at 5,000 connections, roughly 7 KB per connection. Zero worker crashes. The write phases inserted about one million rows in total; a separate check fired 40 concurrent `POST`s at a 4-worker cluster and all 40 rows landed with no errors.

The `GET /api/todos` gap between Part B (27k req/s) and Part A (69k req/s) is large, but the two runs used different methods, so the report does not attribute it to the engine changes.

### Open item from that run: read errors at 5,000 connections

Two benchmarks at 5,000 connections reported `wrk` read errors: `GET /` with 1,997 errors over 820,173 requests (0.24%), and `POST` with 2,386 over 325,224 (0.73%). `GET /api/todos` had none. Each error is one connection dropped and re-opened by `wrk`, and throughput was not visibly affected. Checked at the time: no worker crashed, the file-descriptor limit was 1,048,576, and the kernel's TCP drop and listen-queue-overflow counters stayed at zero. Eighteen shorter runs (3 seconds at 500, 2,000 and 5,000 connections) produced no errors, and repeating an 8-second run at 5,000 connections three times gave errors in one of the three. The cause is unknown.

---

## C. The 2026-09-21 `stress_test.sh` run (`full_run_output.txt`)

Do not quote this run. Two problems, both in how the script was run, not in the server:
- **`GET /` measured a 404.** The script starts `examples/todo_sqlite/cexpress_demo` from the repository root, where `public/` does not exist; the log ends with `app_serve_static: root directory "public" does not exist, not registered`, and `GET /` answers `404` with a 9-byte body. The `GET / (Todo UI)` rows (55,340 / 54,366 / 53,314 req/s) therefore describe a 404 response. (The `/api/todos` and `POST` rows are not affected: those routes do not touch `public/`.)
- **The memory rows are not credible.** They report 7 to 12 MB total across five processes during the 5,000-connection phases, against 134 MB in the direct measurement above, and the `time -l` footer shows 0.01 s of user CPU for a 230 s run. The cause was not investigated.

The `Connection: close` churn phase also produced 0 requests at 1,000 connections (all 1,000 `connect` attempts failed; macOS ephemeral ports stuck in TIME_WAIT) and 4,378 connect errors at 5,000.

---

## Issues found and fixed during testing (Part B)

The stress test surfaced two real problems that ordinary manual testing had not.

### 1. Worker processes killed by `SIGPIPE` (`lib/connection.c`)

- **Symptom:** the cluster master logged
  `Worker 0 (PID …) terminated by signal 13, respawning...` under load.
- **Cause:** a `write()` to a socket the client had already closed raises
  `SIGPIPE`, which terminates the process by default. `SIGPIPE` was only ignored
  inside `tls_init_app`, which does nothing when TLS is off, so plaintext servers
  were unprotected.
- **Why it was easy to miss:** the master respawns crashed workers, so the
  service stayed up.
- **Fix:** `signal(SIGPIPE, SIG_IGN)` now runs at the top of `app_listen_worker`
  for every serving process. The TLS-side call stays too, because `test_tls`
  relies on it (it never goes through `app_listen_worker`).
- **Verified:** that run had zero `terminated by signal` messages.

### 2. Truncation warning printed on every request (`examples/todo_sqlite/db.c`)

- **Symptom:** `db_list_todos: TODO_LIST_MAX exceeded, truncating` flooded the
  terminal.
- **Cause:** the write benchmark grows the table well past the 256-row list cap.
  Once it did, every `GET /api/todos` printed the warning, one stderr write per
  request. Unlike the other `MAX_*` caps in the project, which trigger on one
  unusual request, this cap is reached by ordinary growth of stored data.
- **Fix:** the warning now prints once per process.
- **Verified:** 50 `GET`s against a 300-row table produce one warning line
  instead of 50. The effect of the flood on throughput was not measured in
  isolation, so no speed-up is claimed for this fix.
- **Related script fix:** `scripts/stress_test.sh` used to interleave read and
  write benchmarks per concurrency level, so later reads ran against a table
  already grown past the cap. It now runs all reads first against the small
  seeded table, then all writes.

---

## Reproducing

Manual, as in Part A (from the repository root, then `examples/todo_sqlite/`):

```bash
make demo
cd examples/todo_sqlite
QUIET=1 WORKERS=4 ./cexpress_demo &
# seed 20 todos (see todo.md), then:
wrk -t8 -c100  -d15s http://127.0.0.1:8080/ping
wrk -t8 -c100  -d15s http://127.0.0.1:8080/api/todos
API_KEY=my-secret-api-key wrk -t8 -c100 -d15s -s ../../scripts/wrk_create_todo.lua http://127.0.0.1:8080/api/todos
```

Scripted (fix the two problems in Part C first, or expect the same output):

```bash
scripts/stress_test.sh                                   # full sweep, defaults, memory tracking on
WORKERS=8 CONNS="100 1000 5000 10000" scripts/stress_test.sh
MEASURE_MEMORY=0 scripts/stress_test.sh                  # throughput only
```

Requires `wrk` (`brew install wrk`). Tunable via `PORT`, `WORKERS`, `THREADS`, `DURATION`, `CONNS`, `PHASES`, `DB_PATH`, `API_KEY` and `MEASURE_MEMORY`. Details are in `scripts/CLAUDE.md`.
