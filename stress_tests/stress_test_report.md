# CExpress Todo API — Stress Test Report

## Summary

The SQLite-backed Todo CRUD demo was load-tested with `wrk` against a 4-worker
cluster, with memory tracked throughout.

- **Throughput:** about 58k req/s for the static Todo UI, 27k req/s for
  SQLite-backed reads, and 22k req/s for SQLite-backed writes, and it stays flat
  as connections go from 100 to 5,000.
- **Memory:** the whole server (master plus 4 workers) idles at about **12 MB**
  and stays at **13 MB** while serving 58k req/s over 100 connections. At 5,000
  simultaneous connections it peaks around **50 MB** summed across all five
  processes; the largest single process peaked at 45 MB.
- **Stability:** zero worker crashes across the full run.
- **Open item:** at 5,000 connections `wrk` reported intermittent read errors
  (0.2–0.7% of requests) in some longer runs. The cause was not found (see
  "Open item").

## Setup

| | |
|---|---|
| Machine | Apple M3 Pro, 12 cores, 18 GB RAM, macOS |
| Build | `gcc-16 -O2`, release (`make`) |
| Server | `QUIET=1 WORKERS=4` (multi-process cluster, `SO_REUSEPORT`) |
| Database | SQLite file, WAL mode, `busy_timeout=5000`, one private connection per worker |
| Load generator | `wrk 4.2.0`, 8 threads, keep-alive, on the same machine as the server |
| Duration | 15s per benchmark |
| Connections | 100, 1,000, 5,000 |
| Dataset | 20 seeded todos for read benchmarks; write benchmarks run last |
| Auth | writes send `Authorization: Bearer <API_KEY>` |

Raw output of this run: [`full_run_output.txt`](full_run_output.txt).

## Throughput

| Endpoint | Connections | Requests/sec | Avg latency | Max latency |
|---|---|---|---|---|
| `GET /` (Todo UI, streamed from disk) | 100 | 58,336 | 1.64 ms | 7.6 ms |
| `GET /` (Todo UI, streamed from disk) | 1,000 | 57,953 | 17.2 ms | 53.1 ms |
| `GET /` (Todo UI, streamed from disk) | 5,000 | 54,503 | 61.4 ms | 153.1 ms |
| `GET /api/todos` (SQLite read) | 100 | 27,250 | 3.50 ms | 9.2 ms |
| `GET /api/todos` (SQLite read) | 1,000 | 26,989 | 36.9 ms | 70.7 ms |
| `GET /api/todos` (SQLite read) | 5,000 | 25,575 | 171.8 ms | 378.3 ms |
| `POST /api/todos` (SQLite write) | 100 | 22,183 | 5.02 ms | 158.0 ms |
| `POST /api/todos` (SQLite write) | 1,000 | 22,162 | 44.9 ms | 86.3 ms |
| `POST /api/todos` (SQLite write) | 5,000 | 21,615 | 131.0 ms | 351.4 ms |

Throughput barely moves between 100 and 5,000 connections; only latency grows,
which is queueing (average latency is roughly connections divided by
throughput) rather than degradation. Writes hit the same SQLite file from four
processes concurrently and stay consistent: a separate check fired 40 concurrent
`POST`s at a 4-worker cluster and all 40 rows landed with no errors. The write
phases inserted roughly one million rows in total.

## Memory

Memory was measured two ways, because in cluster mode `/usr/bin/time -l` alone
is misleading: its "maximum resident set size" is the largest *single* process,
not the sum. The script therefore also samples `ps` every 250 ms for the
resident set size (RSS) of the master and every worker, and records the peak sum
per benchmark. (MB here means 1024 × 1024 bytes.)

| Moment | Total across 5 processes | Largest single process |
|---|---|---|
| Idle, after startup and seeding | 11.9 MB | — |
| `GET /` at 100 connections | 12.8 MB | 3.3 MB |
| `GET /api/todos` at 100 connections | 13.0 MB | 3.5 MB |
| `GET /` at 1,000 connections | 20.2 MB | 10.7 MB |
| `GET /api/todos` at 1,000 connections | 16.8 MB | 10.4 MB |
| `GET /` at 5,000 connections | 43.1 MB | 36.7 MB |
| `GET /api/todos` at 5,000 connections | 48.8 MB | 42.4 MB |
| `POST` at 100 / 1,000 connections | 51.5 MB | 45.0 MB |
| `POST` at 5,000 connections | 44.2 MB | 37.8 MB |

What the numbers show:

- **Idle cost is tiny.** Five processes together take about 12 MB, roughly
  2.4 MB each.
- **Serving 58k req/s from 100 connections adds about 1 MB.** Memory is driven by
  the number of open connections, not by request rate.
- **Growth with connections is roughly linear.** Going from 100 to 5,000
  connections adds about 36 MB in total, or around 7 KB per connection. That is
  consistent with the 8 KB initial receive buffer (`BUF_SIZE`) each connection
  allocates, though this run did not isolate that cause.
- **The `POST` rows are not caused by the writes.** RSS is a high-water mark that
  the allocator does not return to the OS, so the `POST` phases inherit the
  memory grown during the earlier 5,000-connection read phases. That is also why
  the total after all benchmarks was still 44.2 MB.

`/usr/bin/time -l` for the whole server run (135.9 s wall clock):

| | |
|---|---|
| Maximum resident set size | 47,218,688 bytes (45.0 MB), largest single process |
| CPU time | 59.9 s user + 60.8 s system, summed over master and workers |
| Swaps | 0 |

The 45.0 MB maximum matches the largest single process seen by the `ps`
sampler, which cross-checks the two methods. `time -l` also prints instructions
retired, cycles and peak memory footprint, but in cluster mode those cover only
the master process, so they are not quoted here.

**Caveats.** Summed RSS counts pages shared between processes (the binary, libc,
SQLite, copy-on-write pages from the fork) once per process, so the totals
overstate real physical memory; the largest-single-process figure is the more
conservative per-worker number. The load generator ran on the same machine, so
it competed with the server for CPU.

## Open item: read errors at 5,000 connections

Two benchmarks at 5,000 connections reported `wrk` read errors: `GET /` with
1,997 errors over 820,173 requests (0.24%), and `POST` with 2,386 over 325,224
(0.73%). `GET /api/todos` at 5,000 connections had none. Each error is one
connection dropped and re-opened by `wrk`, and throughput was not visibly
affected.

What was checked: no worker crashed (0 during the run), the file-descriptor limit
is 1,048,576, and the kernel's TCP drop and listen-queue-overflow counters stayed at zero. Eighteen
shorter runs (3 seconds at 500, 2,000 and 5,000 connections) produced no errors,
and repeating an 8-second run at 5,000 connections three times gave errors in one
of the three. So the errors appear mid-run in longer, high-connection runs and
are intermittent. The cause is unknown and has not been narrowed to the server
rather than the load generator or the operating system. Until it is understood,
the 100 and 1,000 connection figures are the cleaner numbers to quote.

## Issues found and fixed during testing

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
- **Verified:** the full run above had zero `terminated by signal` messages.

### 2. Truncation warning printed on every request (`app/db.c`)

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

## Reproducing

```bash
scripts/stress_test.sh                                   # full sweep, defaults, memory tracking on
WORKERS=8 CONNS="100 1000 5000 10000" scripts/stress_test.sh
MEASURE_MEMORY=0 scripts/stress_test.sh                  # throughput only
```

Requires `wrk` (`brew install wrk`). The script builds the binary, starts the
server under `/usr/bin/time -l` (macOS) or `-v` (Linux), seeds 20 todos, runs the
benchmarks while sampling per-process memory, prints a memory summary and the
`time` report at shutdown, and removes the scratch database on exit. Tunable via
`PORT`, `WORKERS`, `THREADS`, `DURATION`, `CONNS`, `DB_PATH`, `API_KEY`, and
`MEASURE_MEMORY`. Details are in `scripts/CLAUDE.md`.
