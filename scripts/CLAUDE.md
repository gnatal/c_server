# scripts/ — performance & benchmarking tools

## Architecture
Utility and load-testing scripts used to benchmark latency, throughput, and memory stability of the server under high concurrency, and to keep the docs in sync with the code:
- `wrk_create_todo.lua`: Lua request generator for `wrk` benchmarking `POST /api/todos` (Todo CRUD demo, `examples/todo_sqlite/`) - reads the `API_KEY` env var (`os.getenv`, falls back to the server's default) so the bearer token can be kept in sync with whatever the server under test is actually configured with, rather than every write 401ing against a mismatched hardcoded token.
- `stress_test.sh`: End-to-end orchestration - `make demo` (builds `examples/todo_sqlite/cexpress_demo`), boots it in cluster mode (`WORKERS`, default 4) against a scratch SQLite file (`DB_PATH`, default `stress_todos.db`, deleted on exit via a `trap ... EXIT` alongside the server process itself), seeds 20 todos, then runs `wrk` in phases (`PHASES`, default `"ping churn read write"`). Tunable entirely via env vars (`PORT`/`WORKERS`/`THREADS`/`DURATION`/`CONNS`/`PHASES`/`DB_PATH`/`API_KEY`/`MEASURE_MEMORY`) - no flags to remember. Phases:
  - **`ping`** = `GET /ping` over keep-alive connections (fixed 4-byte reply defined inline in `examples/todo_sqlite/main.c`: no DB, no JSON, so it measures the engine's connection and request path alone). Runs first because it never touches the todo table.
  - **`churn`** = the same endpoint with `Connection: close`, one new TCP connection per request, stressing accept/close. Churn leaves a TIME_WAIT socket per connection; the 2026-09-21 run got 0 requests at 1000 connections (all 1000 `connect` errors) and 4,378 connect errors at 5000, so shorten `DURATION` or let TIME_WAIT drain between runs before trusting a churn number.
  - **`read`** = `GET /` (Todo UI) and `GET /api/todos` (SQLite read path) at each concurrency level in `CONNS` (default `100 1000 5000`).
  - **`write`** = `POST /api/todos` (SQLite write path, via `wrk_create_todo.lua`) at each level.
  **Phase ordering matters**: reads run first, in their own pass, against the small seeded 20-row table - each `POST` benchmark inserts tens of thousands of rows, so interleaving read and write phases per concurrency tier made later read benchmarks measure against a table already grown past `TODO_LIST_MAX` (`examples/todo_sqlite/todo_types.h`), silently truncating every list response.
- `docker_stress_test.sh`: the same idea as `stress_test.sh`, same phase/output/env-var pattern, but inside Docker to exercise the real Linux io_uring backend from a Mac. Builds the server image (`Dockerfile`), starts it on a private Docker network with `--security-opt seccomp=unconfined --ulimit memlock=-1:-1` (io_uring-related flags the script chose; whether the default Docker profile blocks io_uring was not tested), and runs `wrk` as its own container on that same network so benchmark traffic goes container-to-container, not through Docker Desktop's localhost proxy (only the readiness wait and seeding use the host port mapping). `wrk` itself runs from a locally-built image (`scripts/wrk.Dockerfile`, compiled from source at `docker build` time) rather than a pulled one - a pulled amd64-only image would run under QEMU emulation on Apple Silicon, making the load generator the bottleneck. Memory tracking sums `/proc/<pid>/status` VmRSS across every `cexpress_demo` process inside the server container via `docker exec` (PID 1 is always the master - the Dockerfile's `CMD` is exec-form, no shell wrapper), sampled every 500ms rather than `stress_test.sh`'s 250ms since each sample is a real process spawn through containerd/runc; there is no `/usr/bin/time -l` equivalent. **The first real run of this script (2026-09-22) found C6** (`improvements.md`, `lib/CLAUDE.md` "Known gaps"): the io_uring backend closes every keep-alive connection after its first request, so every phase's numbers currently reflect that bug (`wrk` read-error counts exceeding the request count) until C6 is fixed - the script itself works correctly, it just faithfully exposes a real, previously-unverified server bug (nothing in this project had run on real Linux before this).
- `check_docs.sh` (`make check-docs`): fails when a `lib/*.h` engine header declares a public function that `lib/API.md` never mentions, or when `lib/API.md` names a `name(` that no engine header or the vendored yyjson header declares. The yyjson API is not required to be listed (it has hundreds of functions); only the subset named in `API.md` is checked for typos. Keeps the LLM-facing API index from drifting.
- `export_framework.sh` (`make export DEST=...`): copies `lib/` into another directory together with a standalone Makefile and README (see `../importing.md`). The generated Makefile lists the current sources (arena, yyjson, picohttpparser, the per-OS event loop: on Linux the dispatcher plus both the io_uring and epoll backends, or epoll only with `NO_URING=1`) and links `-luring` on Linux unless `NO_URING=1`. TLS is out of scope for this library (terminate it at a gateway/reverse proxy in front); the exported Makefile has no OpenSSL detection.

## Known problems with `stress_test.sh`
- **Fixed 2026-09-22: it used to start the demo from the repository root.** The demo resolves `public/` and the
  default `todos.db` against its working directory, so from the root `app_serve_static` logged `root directory
  "public" does not exist, not registered` and `GET /` answered `404` with a 9-byte body - the `GET / (Todo UI)`
  rows in `stress_tests/full_run_output.txt` (captured 2026-09-21, before this fix) measure that 404, not the
  6,481-byte page. Fix: the script still builds and does its own `cd` from the repository root (the Makefile paths
  are root-relative), but now launches the server itself from a `DEMO_DIR="examples/todo_sqlite"` subshell
  (`cd "$DEMO_DIR"; VAR=... exec ... ./cexpress_demo` - note the env-var assignments must precede `exec`, not
  follow it as an argument to it, which silently fails with "exec: QUIET=1: not found" since `exec` has no such
  option). `DB_PATH`'s pre-start and cleanup `rm -f` calls were updated to `$DEMO_DIR/$DB_PATH` to match, since the
  scratch database now lives there too, not at the repository root. Verified: `PHASES=read DURATION=2s CONNS=10
  MEASURE_MEMORY=0 ./scripts/stress_test.sh` now reports `GET / (Todo UI)` transferring ~6.27 KB/request
  (850.83 MB / 135,773 requests), matching the real page size; `MEASURE_MEMORY=1` (the `time(1)`-wrapped,
  `pgrep -P`-based master-PID resolution path) was also re-verified working end to end after the subshell change.
- **The memory rows from the 2026-09-21 run are not credible** (separate, still-open problem - not touched by the
  fix above). They report 7-12 MB total across 5 processes during the 5,000-connection phases; a direct measurement
  (below) puts one worker with 5,000 idle connections at about 121 MB RSS. The `time -l` footer also shows 0.01 s
  user CPU for a 230 s run. The cause was not investigated (the sampler and the `time`/`pgrep -P` PID resolution are
  the places to look).

## Memory tracking (`stress_test.sh`, on by default, `MEASURE_MEMORY=0` to disable)
Two independent measurements, because neither alone is accurate for a forked cluster:
- **`/usr/bin/time -l`** (macOS; `-v` on Linux) wraps the server. Its rusage is collected by `wait4` on the master, which has reaped the workers, so `maximum resident set size` (bytes) is the **largest single process**, not the sum, while user/sys CPU time is summed across master and workers. `instructions retired`, `cycles elapsed` and `peak memory footprint` cover the master only and must not be quoted as whole-server figures. Under `time`, `$!` is `time`'s own PID, so the script resolves the real master with `pgrep -P` and signals that instead - `SIGTERM` sent to `time` itself would kill it before it prints the report. The server's stderr (and therefore `time`'s report, and any `terminated by signal` worker-crash messages from `cluster.c`) is redirected to a temp log printed at the end; the script reports the crash count from it.
- **A `ps`-based sampler** (every 250 ms, per benchmark) sums RSS across the master and all its children and records the peak total plus the peak single process. This is the whole-server number; the single-process peak should agree with `time -l`'s maximum RSS, which is the cross-check (it did not in the 2026-09-21 run, see above).
- **RSS is a high-water mark**: the allocator does not return freed pages to the OS, so a later benchmark inherits memory grown by an earlier, higher-connection one. Summed RSS also counts shared pages (binary, libc, SQLite, fork copy-on-write) once per process, so totals overstate physical memory.
- **Per-connection cost** (direct measurement, macOS, one worker, N idle keep-alive connections opened from a script, RSS from `ps`): 5,000 connections took about 121 MB, about 25 KB each, whether idle or after one `GET /ping` each. That is the 8 KiB `in_buf` plus the touched part of the 64 KiB per-connection arena (`../tradeoffs.md`). The earlier engine measured about 7 KB per connection.
- Known open item: at 5,000 connections `wrk` intermittently reports read errors (0.2-0.7% of requests) in longer runs; seen again on 2026-09-21 in `ping` (2,219 / 1,624 errors), `GET /` and `POST`; no worker crashes, cause unidentified.

## Execution & Concurrency
- Designed to test keep-alive connection reuse, routing/middleware overhead, and SQLite read/write throughput under high concurrent connection loads (e.g. 5,000 connections over 8 threads) and multi-process cluster contention (`stress_test.sh`'s default `WORKERS=4` is the scenario `lib/CLAUDE.md`'s "Behavior reference, Workers and fork" fork-safety fix specifically has to hold up under - multiple worker processes hitting the same SQLite file concurrently).
- Benchmarking should run against a release-optimized build with keep-alive active to verify zero TCP round-trip latency anomalies (avoiding Nagle/delayed ACK interaction). The demo sets `TCP_NODELAY` on accepted sockets.
- `wrk` runs on the same machine as the server in every recorded run, so both compete for the same cores; on the M3 Pro a single worker reached 221k `/ping` req/s against 250k for four workers, so those runs are client-bound. Only compare numbers taken the same way.
- `stress_test.sh` requires `wrk` on `PATH` (`brew install wrk` / `apt install wrk`) and exits early with a clear message if it's missing.

- Equivalent manual commands (what `stress_test.sh` automates; run them from `examples/todo_sqlite/` after `make demo` from the repository root):
```
QUIET=1 WORKERS=4 ./cexpress_demo
wrk -t8 -c5000 -d15s http://127.0.0.1:8080/api/todos
wrk -t8 -c1000 -d15s http://127.0.0.1:8080/api/todos
wrk -t8 -c100 -d15s http://127.0.0.1:8080/api/todos
wrk -t8 -c1000 -d15s http://127.0.0.1:8080/ping                              # keep-alive connections
wrk -t8 -c1000 -d15s -H "Connection: close" http://127.0.0.1:8080/ping       # connection churn
API_KEY=my-secret-api-key wrk -t8 -c100 -d15s -s ../../scripts/wrk_create_todo.lua http://127.0.0.1:8080/api/todos
```
