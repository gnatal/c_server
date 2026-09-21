# Findings: review of the project's Markdown files

> **Status (2026-09-21, after the documentation update).** Section 4 below is the original review. Every stale claim it lists has since been fixed in the docs, the header comments and the doc-related scripts, and the doc examples that were checked were compiled and run. One exception: on `docs/index.html` only the headline figures, the memory and hot-path tabs and the false architecture claims were refreshed; the framework-comparison chart and the SQLite, static-file and churn tabs still show numbers from earlier revisions (the page now says so), because no competitor was re-run. What the update found in the *code* (not fixed, documented as known gaps) is in the next section. Two corrections to the original review: `AGENTS.md` is a symlink to `CLAUDE.md` (one file, not two copies), and `ping.c/h` were deleted in the "Isolating the project" commit (the `/ping` handler is now inline in `examples/todo_sqlite/main.c`; the docs had kept pointing at the old files).

## What the update found beyond the original review

Verified against a running server or a scratch program; none of it is fixed:

1. **A malformed request line gets no response.** `GET /\r\n\r\n`, `GET / HTTP/2.0` and plain garbage leave the connection open (no 400) until 8 KiB arrive (431) or the 60 s idle timeout fires (408). Cause: `request_is_complete` checks `header_len == 0` before it checks whether `request_framing` returned an error. Likely fix: test `content_length < 0` first. The existing test for "malformed 400" uses a bad `Content-Length`, and the fuzzer only checks memory safety, so neither catches it.
2. **Path-parameter names are stored per tree position.** `/orders/:id/items` then `/orders/:oid/notes`: `req_get_param(req, "oid")` returns `NULL`.
3. **`scripts/stress_test.sh` starts the demo from the repository root.** `public/` is not found, so `GET /` measures a 404 and `/static` is not registered. Its recorded memory rows (7 to 12 MB at 5,000 connections) also contradict a direct measurement (about 134 MB).
4. **On macOS, `SO_REUSEPORT` did not balance connections across the 4 workers** in the one test run: one worker held about 93% of the resident memory at 5,000 connections. Linux was not measured.
5. **Per-connection memory is about 25 KB resident on macOS** (72 KB allocated), against about 7 KB before the arena.
6. **The epoll and io_uring backends issue a syscall on every interest change**; only kqueue uses `events_watched` to skip it (the old docs claimed both). Header parsing runs three times per request.
7. **`make check-docs` was failing** (the yyjson header had been added to its scan); the script now checks engine headers only. `scripts/export_framework.sh` generated a Makefile that listed the deleted `lib/json/*.c` and epoll; fixed.
8. **The Docker build does not run the test suite** (the README claimed it did). io_uring blocked by a container runtime makes the server exit at startup.
9. **Parser limits changed silently**: a 33rd request header is a 400 (it used to be dropped); bare `\n` line endings are accepted.

Verified as working on macOS on the same date: `make test` (14 suites), the same suites under ASan + UBSan, a 200,000-iteration fuzz run, `make check-docs`, and the README's run, TLS and CRUD `curl` commands. Not run: anything on Linux.

---

## Original review

Scope: all 14 `.md` files tracked in the repo (about 2,300 lines). Where a doc made a claim that was cheap to check, I compared it with the code (file existence, `Makefile`, a few greps). I did not build the project or run any tests.

## 1. What the project is

CExpress is an Express.js-style HTTP/1.1 server framework in C11. The design is single-threaded and non-blocking inside each process, and it scales by forking N worker processes that share one port through `SO_REUSEPORT`. A master process respawns crashed workers and coordinates a 5 s graceful drain. State is per process, with no shared memory and no locks.

- `lib/` builds `libcexpress.a`: router, middleware pipeline, HTTP parser, response builder, static files, multipart/urlencoded parsers, TLS, and event-loop backends.
- `examples/todo_sqlite/` is a reference app, a Todo CRUD API on SQLite with a single-page UI.

## 2. The documents, by role

| File | Role |
|---|---|
| `CLAUDE.md`, `AGENTS.md` | Project rules for coding agents. `AGENTS.md` is a symlink to `CLAUDE.md`. |
| `lib/CLAUDE.md` | The best engine reference. It covers request lifecycle, limits, an ownership table, return conventions, hot-path rules, and known gaps. |
| `lib/API.md` | One-line index of every public function, checked by `make check-docs`. |
| `README.md`, `DOC.md` | User-facing overview and API guide. |
| `concurrency.md` | Multi-worker architecture, Docker/PID 1 behavior, crash recovery, shutdown. |
| `importing.md` | How to vendor CExpress into another project, with a consumer Makefile and an `AGENTS.md` template. |
| `tradeoffs.md` | Rationale for the arena allocator, yyjson, and picohttpparser. The newest document. |
| `examples/todo_sqlite/CLAUDE.md` | Demo app architecture, fork-safe SQLite, handler conventions. |
| `tests/CLAUDE.md`, `scripts/CLAUDE.md` | Test harness and benchmark tooling. |
| `stress_tests/stress_test_report.md` | Results of one `wrk` run plus two bugs found by it. |
| `todo.md` | Despite the name, a how-to-run cheat sheet for the demo. It is not a task list (the file now says so at the top; the name is unchanged). |

## 3. Key technical content

**Request path** (`lib/CLAUDE.md`): `recv` into `in_buf`, then `request_is_complete`, `parse_http_request`, `match_route`, `dispatch` (middleware then handler), `res_*` builds bytes into `out_buf`, then `flush_connection`. Only `connection.c`, `event_loop_*.c`, `tls.c` and `cluster.c` do I/O, so parsing, routing and response building can be tested without sockets.

**Security posture:**
- Deny by default: 404, 405 with `Allow`, and 403 for path traversal.
- Requests that mix `Content-Length` and chunked encoding are rejected (400).
- Headers are capped at 8 KiB (431), bodies at 10 MiB (413), and paths at 255 bytes (414).
- Control characters in response headers, cookies and redirects are dropped, which defends against response splitting.
- Static files check both `..` and the `realpath` result.
- A 60 s idle timeout applies to connections.

**Ownership rules** are tabulated in `lib/CLAUDE.md`. Handlers never free `Request.body` or `req_get_*` results. The engine frees them after dispatch.

**Fork safety:** never open a DB or socket before `app_listen`. Register `app_on_worker_start` and open the handle there. The demo does this with `db_open` for validation, then `db_close`, then `db_worker_init` per worker, with WAL and a busy timeout.

**Verification targets:** `make test`, `make SANITIZE=1 BUILD_DIR=build-asan test`, `make fuzz`, `make bench`, `make check-docs`.

**Performance claims:**
- README: about 287k req/s on `/ping` with 4 workers, and 207k with 1 worker.
- `lib/CLAUDE.md`: the pure request path costs 0.39 µs for a minimal GET.
- Stress report: 58k req/s for the UI, 27k for SQLite reads, 22k for SQLite writes, about 12 MB idle, and about 50 MB at 5,000 connections.

**Known gaps** listed in `lib/CLAUDE.md`, all verified but unfixed:
- HTTP pipelining is dropped: a second request already in the buffer is discarded.
- Chunked bodies are re-scanned on every `recv`, which is quadratic in the worst case.
- Each connection allocates an 8 KiB `in_buf` up front.
- `Request.body` is a malloc'd copy per request.
- Silent truncation of long queries, header values and params.
- No HTTP/2, `Expect: 100-continue`, compression, `Range`, or WebSocket.

**Open item** (stress report and `README.md`): `wrk` shows intermittent read errors at 5,000 connections, and the cause is unidentified. Also unexplained: `GET /` gives the same throughput with 1 or 4 workers, and the script's `/ping` numbers run lower than the manual run.

## 4. Main finding: much of the documentation is stale

`tradeoffs.md` and the git log (`Using yyjson instead of my own json`, `arena allocator ... vendor picohttpparser`, `Adding patricia-tree`, `Linux optimization`, `removing outdated files`) show the engine was substantially reworked. Most other docs were not updated. Checked against the code:

| Doc claim | Reality in the repo |
|---|---|
| A custom JSON library in `lib/json/` (`json_parse`, `JsonWriter`/`jw_*`, `json_new_*`, `json_stringify`), documented in `README.md`, `DOC.md`, `importing.md`, `lib/API.md` and `lib/CLAUDE.md` | `lib/json/` no longer exists. `lib/vendor/yyjson` is used, allocating from the per-connection arena. `handlers.c` calls `yyjson_mut_*` directly. |
| Handwritten HTTP parser; `lib/CLAUDE.md` hot-path rules ("`memchr`, no `strtok`") | `lib/vendor/picohttpparser` is now the parser (`tradeoffs.md` and the `Makefile` agree). |
| `lib/CLAUDE.md` ownership table: `Request.body` is malloc'd and freed by the engine; `out_buf` is malloc'd per response | `tradeoffs.md` says allocations now come from a 64 KB per-connection bump arena (`lib/arena.c`) reset per request. The ownership table is probably wrong now. I did not audit the code. |
| Linear route matching, at most 32 routes | `lib/app_types.h` now defines a `PatriciaNode` radix tree, and `router.c` references it. No doc mentions it. |
| Event loops are kqueue and epoll only | The Linux build in the `Makefile` uses `lib/event_loop_io_uring.c` and links `-luring`. No doc mentions it. The README still says `epoll`. |
| README says 15 test suites, including `test_json`; `tests/CLAUDE.md` documents `test_json.c` | `tests/test_json.c` is gone, though the `Makefile` still defines `JSON_TEST_BIN` for it. |
| `GET /ping` handler in `examples/todo_sqlite/ping.c/h` and `tests/test_ping.c`; several docs, and `scripts/CLAUDE.md`'s benchmarks, use `/ping` | `ping.c` and `ping.h` do not exist in the tree. `tests/test_ping.c` does. |
| `lib/json/CLAUDE.md` is referenced from README, `lib/CLAUDE.md` and the demo docs | The file does not exist. |
| Stress/benchmark numbers in README, the report and `lib/CLAUDE.md` | They predate the arena/yyjson/picohttpparser/io_uring changes. |
| Memory: about 7 KB per connection, from the 8 KiB `BUF_SIZE` | `tradeoffs.md` says 64 KB per connection, about 640 MB for 10k idle connections. The two documents contradict each other, and the stress report's memory table is now inconsistent with the design. |
| `DOC.md` says the worker hook example is in `app/db.c`; `stress_test_report.md` mentions `app/db.c` | The path is now `examples/todo_sqlite/db.c`. |

**Other issues:**
- `CLAUDE.md` (and so `AGENTS.md`, its symlink) contained the "Where to start" section twice.
- The "Write tests for all new code" bullet in `CLAUDE.md` and `AGENTS.md` is a copy of the "Isolate side effects" bullet. It says nothing about tests.
- `DOC.md` and `README.md` overlap heavily and are only partly consistent with `lib/API.md`. For example, `DOC.md`'s "index" table omits many functions that `lib/API.md` lists.
- `todo.md` has a misleading name (see the table in section 2).
- `stress_test_report.md` links `full_run_output.txt`, which does exist in `stress_tests/`.
- Working-tree clutter in `examples/todo_sqlite/`: the tracked files include `cexpress_demo` (a binary) and a second `Makefile`. `.o` files, `test.db` and `todos.db*` are also present, and I did not check whether they are tracked. The docs say builds go to `build/` only.

## 5. Suggested follow-ups

1. Update `lib/CLAUDE.md` first, since it is the file agents are told to trust. Rewrite it for picohttpparser, yyjson plus arena ownership, the Patricia router, and io_uring on Linux.
2. Update `lib/API.md`, and confirm `make check-docs` still passes and still covers the yyjson-based API.
3. Remove or rewrite the JSON sections in `README.md`, `DOC.md` and `importing.md`. Their code samples use an API that no longer exists, so they would fail to compile.
4. Fix the test-suite list and the `Makefile` `test_json` and `test_ping` references. Restore `ping.c` or remove the `/ping` docs.
5. Re-run `make bench` and the stress test, then refresh the numbers. Reconcile the memory-per-connection figures.
6. Deduplicate `CLAUDE.md` and `AGENTS.md`, and fix the tests bullet.
7. Decide whether `README.md` or `DOC.md` is the canonical API guide, and trim the other.
