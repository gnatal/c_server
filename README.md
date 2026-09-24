# CExpress

A lightweight, high-performance HTTP/1.1 server and web framework written entirely in C (C11), designed with a developer experience inspired by [Express.js](https://expressjs.com/). Each process runs a single-threaded, non-blocking event loop; a cluster of such processes scales across cores.

> [!NOTE]
> **Library Architecture:**
> The code in `lib/` compiles to a reusable static library (`build/lib/libcexpress.a`). The code in `examples/todo_sqlite/` is a reference implementation — a SQLite-backed Todo CRUD API — showing how to consume the library. See [`examples/todo_sqlite/CLAUDE.md`](examples/todo_sqlite/CLAUDE.md) for its architecture.
>
> **Where the rest of the docs are:** [`DOC.md`](DOC.md) (using the API), [`lib/API.md`](lib/API.md) (every public function, one line each), [`lib/CLAUDE.md`](lib/CLAUDE.md) (engine internals, limits, ownership, known gaps), [`importing.md`](importing.md) (using CExpress from another project), [`concurrency.md`](concurrency.md), [`tradeoffs.md`](tradeoffs.md) (why the engine is built this way, and what it costs).

---

## Table of Contents
- [Purpose](#purpose)
- [Performance & Benchmarks](#performance--benchmarks)
- [Architecture Overview](#architecture-overview)
- [Features](#features)
- [Known Gaps](#known-gaps)
- [Prerequisites](#prerequisites)
- [Building & Running](#building--running)
- [Running Tests](#running-tests)
- [Configuration](#configuration)
- [Quick Start & Example Usage](#quick-start--example-usage)
- [Project Layout](#project-layout)
- [Roadmap](#roadmap)

---

## Purpose

Modern backend applications often rely on high-level runtimes like Node.js or Go. This project brings the ergonomic, developer-friendly routing and middleware design of **Express.js** directly to **C**, providing:
- **High Performance & Low Latency**: Native execution with minimal CPU overhead, sub-millisecond response times, and about 250,000 requests/sec on a minimal endpoint with 4 workers on an Apple M3 Pro laptop (single run, load generator on the same machine). See [Performance & Benchmarks](#performance--benchmarks).
- **Small Footprint**: `lib/` has no system dependencies beyond standard C and POSIX APIs, and `liburing` on Linux. JSON ([yyjson](https://github.com/ibireme/yyjson)) and HTTP tokenizing ([picohttpparser](https://github.com/h2o/picohttpparser)) are vendored as source in `lib/vendor/`. The bundled demo app additionally links SQLite (embedded, no server process) for its Todo persistence layer.
- **Event-Driven Non-Blocking I/O**: Native `kqueue` on macOS / BSD and `epoll` on Linux (`io_uring` readiness polling opt-in with `CEXPRESS_EVENT_LOOP=io_uring`), following the same architectural pattern as Node.js's underlying `libuv`.
- **Memory Control**: Explicit bounded buffers, aggressive `const` correctness, hard input limits, and a per-connection arena allocator so per-request data needs no individual `free`. The trade is a larger per-connection footprint (see [`tradeoffs.md`](tradeoffs.md)).

---

## Performance & Benchmarks

**Method.** Measured on 21 Sep 2026 with `wrk` (8 threads, keep-alive, 15 s per row) against the demo started as `QUIET=1 WORKERS=4 ./cexpress_demo` from `examples/todo_sqlite/`, on an Apple M3 Pro laptop (macOS, gcc-16 -O2) with `wrk` running on the same machine, so both compete for the same cores. **Each row is a single run**; earlier runs of the same command on this machine varied by several percent, and results move with whatever else the laptop is doing. Linux (io_uring) was not benchmarked.

**`GET /ping`**: a fixed 4-byte reply, no database, no JSON. This is the engine's connection and request path on its own.

| Concurrency | Throughput | Avg Latency | Max Latency | Notes |
|---|---|---|---|---|
| **100 connections** | **250,055 req/sec** | **393 µs** | 19.07 ms | |
| **1,000 connections** | **207,903 req/sec** | **4.81 ms** | 19.54 ms | |
| **5,000 connections** | **238,859 req/sec** | **14.72 ms** | 175.88 ms | 2,219 `wrk` read errors |

With `WORKERS=1` the same test at 100 connections gave 221,611 req/sec (359 µs average), close to the 4-worker figure. Two things follow: these runs are limited by the load generator sharing the machine, and on macOS the workers did not share connections evenly (see [`concurrency.md`](concurrency.md)), so this table says little about multi-core scaling.

**Todo demo endpoints** (4 workers):

| Endpoint | Concurrency | Throughput | Avg Latency | Max Latency |
|---|---|---|---|---|
| `GET /`: the Todo UI, a 6,481-byte HTML page streamed from disk with `res_send_file` | 100 | 60,466 req/sec | 1.60 ms | 19.16 ms |
| `GET /api/todos`: 20 rows, SQLite read, JSON via yyjson | 100 | 69,011 req/sec | 1.39 ms | 5.70 ms |
| `GET /api/todos`: same | 1,000 | 61,999 req/sec | 16.05 ms | 40.73 ms |
| `POST /api/todos`: SQLite write, WAL mode | 100 | 23,846 req/sec | 4.13 ms | 42.85 ms |

At 5,000 connections `wrk` reported read errors in every long run recorded so far (2,219 here on `/ping`; 1,624 to 2,393 in the earlier script run); there were no connect errors and no worker-crash messages in the final run's log. The cause is not identified.

**Pure request path, no sockets** (`make bench`, one core): minimal GET 208 ns, browser-shaped GET (10 headers, cookies, query) 781 ns, JSON POST 315 ns, 404 186 ns; emitting a 20-row JSON list with yyjson 659 ns.

**Memory** (macOS, `ps` RSS): the 5-process cluster idles at 11.5 MB. Each open keep-alive connection costs about 25 KB resident (8 KiB input buffer plus the touched part of a 64 KiB arena): 5,000 connections took about 121 MB on a single worker. The earlier engine measured about 7 KB per connection.

These figures are not comparable with the ones this README carried before the engine rework (for example 287k req/sec on `/ping` at 100 connections): the setup, the engine and the machine's state all differ, and no A/B run of the old commit was made.

> [!TIP]
> Reproduce the tables above manually (this is the setup they were measured with):
> ```bash
> make demo                                   # from the repository root
> cd examples/todo_sqlite
> # Terminal 1: 4-worker cluster, no access logging
> QUIET=1 WORKERS=4 ./cexpress_demo
>
> # Terminal 2:
> wrk -t8 -c100  -d15s http://127.0.0.1:8080/ping
> wrk -t8 -c1000 -d15s http://127.0.0.1:8080/ping
> wrk -t8 -c5000 -d15s http://127.0.0.1:8080/ping
> wrk -t8 -c100  -d15s http://127.0.0.1:8080/        # Todo UI
> wrk -t8 -c100  -d15s http://127.0.0.1:8080/api/todos
> ```
> Seed 20 todos first for the `/api/todos` rows (see the `POST` examples below).
> `scripts/stress_test.sh` automates a sweep, but as of this writing it starts the server from the repository root, where the demo cannot find `public/` (so its `GET /` rows measure a 404) and its memory rows disagree with direct measurements; see [`scripts/CLAUDE.md`](scripts/CLAUDE.md) before trusting its output.

---

## Architecture Overview

The codebase is split into two distinct tiers:

```
┌────────────────────────────────────────────────────────────┐
│   examples/todo_sqlite/ (Demo App: SQLite-backed Todo CRUD)│
│   - main.c (Configures routes, routers & boots server)     │
│   - handlers.c (Todo CRUD route handlers, yyjson output)   │
│   - middlewares.c (Auth, logging, body size guards)        │
│   - db.c (SQLite schema, CRUD, worker-init hook)           │
└───────────────────────────┬────────────────────────────────┘
                            │ links against
┌───────────────────────────▼────────────────────────────────┐
│           lib/ (Core Engine - libcexpress.a)               │
│   - connection.c : Socket I/O, lifecycle, 64 KiB arena     │
│   - arena.c      : Per-connection bump allocator           │
│   - cluster.c/h  : Multi-worker SO_REUSEPORT supervisor    │
│   - event_loop.h : Cross-platform event-loop interface     │
│   - event_loop_kqueue.c   : macOS/BSD kqueue loop          │
│   - event_loop_io_uring.c : Linux io_uring readiness loop  │
│   - event_loop_epoll.c    : epoll backend (opt-in)         │
│   - http_parser.c: Framing, query, dechunking, Request     │
│   - router.c     : Per-method Patricia trees, sub-routers  │
│   - middleware.c : Dispatch & middleware pipeline          │
│   - response.c   : Response builder, chunks & trailers     │
│   - static.c     : Traversal-safe static file serving      │
│   - multipart.c  : RFC 7578 multipart/form-data parser     │
│   - urlencoded.c : application/x-www-form-urlencoded       │
│   - vendor/      : picohttpparser (HTTP), yyjson (JSON)    │
└────────────────────────────────────────────────────────────┘
```

---

## Features

### 1. Express-Style Routing & Sub-Routers
- **Full HTTP Method Suite**: `app_get()`, `app_post()`, `app_put()`, `app_patch()`, `app_delete()`, `HEAD` (RFC 7230 §3.3.3 body suppression), and automatic `OPTIONS` (`Allow:` header generation).
- **Path Parameter Extraction**: Named segment variables (e.g. `/users/:id`, `/orgs/:orgId/repos/:repoName`) accessible via `req_get_param(req, "id")`.
- **Wildcard Matching**: Trailing wildcards (e.g. `/files/*`).
- **Tree Routing**: One Patricia (segment) tree per HTTP method; a literal segment beats `:param` beats `*` regardless of registration order, with backtracking. No fixed route cap per app.
- **Sub-Routers & Prefix Mounting**: Standalone `Router` instances mounted via `app_mount(&app, "/api", &router)` with nested middleware scoping.
- **Deny-by-Default**: Unmatched routes return `404 Not Found`; mismatched methods return `405 Method Not Allowed` with valid `Allow` headers.

### 2. Middleware & Centralized Error Pipeline
- **Sequential Execution**: Middleware functions execute in order with `chain_next(chain)`.
- **Prefix-Scoped Middleware**: `app_use_prefix(app, "/api", mw)` gates entire URL hierarchies.
- **Per-Route Middleware**: `app_get_mw(...)` and `app_post_mw(...)` attach guards to specific routes.
- **Short-Circuiting & Post-Processing**: Early return on failure (e.g. 401/403) or inspection after the handler finishes (e.g. access logging).
- **Centralized Error Handling**: `chain_error(chain, status, message)` mirrors Express's `(err, req, res, next)`.
- **Demo Middlewares** (in `examples/todo_sqlite/`, not the engine): `mw_logger`, `mw_body_size_guard`, `mw_authenticate` (constant-time token verification), and `error_handler_json`.

### 3. Non-Blocking Event-Driven Networking & Multi-Worker Concurrency
- **Multi-Worker Process Model (`SO_REUSEPORT`)**: Dedicated listening socket per worker, no shared state or locks between workers (see [concurrency.md](concurrency.md); connection distribution across workers was uneven on macOS in the one test run).
- **Master Process Supervision**: Automatically reaps dead children, prevents container zombie leaks, respawns crashed workers on the fly, and coordinates clean graceful drains.
- **Cross-Platform Event Loop**: Native `kqueue` on macOS/BSD and `epoll` (with `timerfd` / `signalfd`) on Linux; `io_uring` readiness polling is built in on Linux but only used with `CEXPRESS_EVENT_LOOP=io_uring`, since it measured 20-25% slower than epoll. `make NO_URING=1` builds epoll only.
- **Dynamic Connection Table**: Bounded only by `RLIMIT_NOFILE`, growing dynamically via `ensure_connection_capacity`.
- **HTTP/1.1 Keep-Alive**: Persistent connections with idle connection timeout sweeps.
- **Graceful Shutdown**: Synchronous signal trapping (`SIGINT`/`SIGTERM`), stops accepting new connections, drains in-flight responses, and enforces a 5-second deadline timer before clean exit.

### 4. HTTP/1.1 Parser & Security Guards
- **Vendored tokenizer, engine-owned framing**: picohttpparser parses the request line and headers; `http_parser.c` applies the limits and framing rules on top. Parsing functions are pure and take plain buffers.
- **Chunked Request Dechunking**: Decodes incoming `Transfer-Encoding: chunked` payloads, handling chunk extensions and trailers, and rejecting smuggling attacks (400 if both `Transfer-Encoding` and `Content-Length` are sent).
- **Query String & URL Decoding**: Query extraction (`req_get_query`) and percent-decoding (`url_decode`).
- **Security Limits**: Rejects oversized/negative `Content-Length` (400), more than 32 headers (400), payload overflow (413), path overflow (414), and header overflow / Slowloris patterns (431); idle connections are closed after 60 s.
- **Response-splitting defense**: header names/values, cookies and redirect targets containing control characters are dropped or refused.

### 5. Streaming & Chunked Responses
- **Procedural Chunked Response API**: `res_write(res, data, len)` and `res_end(res)` emit HTTP/1.1 chunked framing (`<hex>\r\n<data>\r\n`) with buffer growth up to about 10 MB while preserving pure handler testability.
- **RFC 7230 Chunked Trailers**: `res_set_trailer(res, name, value)` declares `Trailer:` in headers and outputs trailers after the terminal `0\r\n` chunk.
- **Bounded 16KB File Streaming**: `res_send_file(res, content_type, filepath)` streams files in 16KB chunks directly through `flush_connection` with 64KB cooperative yielding per event-loop turn, never buffering whole files into RAM.

### 6. Static File Serving
- **Mounted Route Serving**: `app_serve_static(app, "/static", "public")` (the root is resolved against the working directory when the route is registered).
- **Path-Traversal Protection**: Pure textual `..` segment rejection paired with realpath canonical containment checks against symlink escapes.
- **Directory Index Fallback**: Automatically serves `index.html` for directory requests.
- **Built-in MIME Registry**: Automatic Content-Type resolution for CSS, JS, HTML, PNG, JPEG, SVG, JSON, and binaries.

### 7. Form & Body Parsers
- **Multipart Form Data**: RFC 7578 parser (`lib/multipart.h`) extracting fields and binary file parts with embedded NUL safety.
- **URL-Encoded Forms**: `application/x-www-form-urlencoded` parser (`lib/urlencoded.h`).

### 8. Cookie Management
- **Cookie Parsing**: Access request cookies via `req_get_cookie(req, "session")`.
- **Cookie Setting & Clearing**: `res_set_cookie(res, name, val, &opts)` and `res_clear_cookie(res, name, path)` supporting `Path`, `Domain`, `Max-Age`, `HttpOnly`, `Secure`, and `SameSite` (`Strict`/`Lax`/`None`).

### 9. JSON (yyjson) and the Per-Connection Arena
- JSON reading and writing use the vendored [yyjson](https://github.com/ibireme/yyjson) library, included by `cexpress.h`. There is no separate engine JSON layer.
- Each connection owns a 64 KiB arena allocated with it. `arena_yyjson_alc(&res->conn->arena)` makes yyjson allocate from it, so documents, the request body and the response bytes need no individual `free`; the arena is reset when a keep-alive response has been written. The one exception: the string returned by `yyjson_mut_write` is libc-allocated and must be `free`d. See [DOC.md](DOC.md#5-memory-model) and [`lib/CLAUDE.md`](lib/CLAUDE.md).

---

## Known Gaps

Verified against the current code on 21 Sep 2026 and not yet fixed (details and suggested fixes in [`lib/CLAUDE.md`](lib/CLAUDE.md), "Known gaps"):
- A request with a malformed request line (no HTTP version, `HTTP/2.0`, garbage) gets **no response**; the connection stays open until 8 KiB arrive or the 60 s idle timeout fires.
- Path parameters are stored per tree position: `/orders/:id/items` plus `/orders/:oid/notes` capture both under `id`.
- HTTP pipelining is dropped: a second request already in the buffer is discarded.
- Chunked request bodies are re-scanned from the start on every `recv`.
- About 25 KB resident per connection on macOS (72 KB allocated), up from about 7 KB before the arena.
- `scripts/stress_test.sh` measures a 404 for `GET /` and its memory sampler output is not credible (see `scripts/CLAUDE.md`).
- No HTTP/2, `Expect: 100-continue`, compression, `Range`, or WebSocket.

---

## Prerequisites

- **Operating System**: macOS / BSD (native `kqueue`) or Linux (epoll; the opt-in io_uring backend needs kernel 5.13 or newer). Docker on any host runs the Linux build.
- **Compiler**: C11 compliant compiler (`gcc-16` on macOS; `gcc` on Linux). The `Makefile` picks one by OS; override with `make CC=<compiler>`.
- **Build Tool**: GNU `make`.
- **liburing** (Linux only): `liburing-dev` on Debian/Ubuntu and on Alpine. The Linux `Makefile` links `-luring`.
- **SQLite development headers**: required to build the demo app (`examples/todo_sqlite/db.c`) — `sqlite-dev` on Alpine, `libsqlite3-dev` on Debian/Ubuntu, or `brew install sqlite` on macOS. The demo's `Makefile` auto-detects a Homebrew keg, falls back to `pkg-config`, then a bare `-lsqlite3`. The engine does not need SQLite.
- **Optional**: Docker (containerized run), and [`wrk`](https://github.com/wg/wrk) for load testing (`brew install wrk` / `apt install wrk`).

> [!NOTE]
> This engine is plaintext HTTP/1.1 only; there is no TLS support. Terminate TLS at a gateway or reverse proxy in front of it (nginx, an ALB, a sidecar).

> [!TIP]
> On macOS, `gcc-16` is installed via Homebrew (`brew install gcc`). The `Makefile` detects macOS (`Darwin`) or Linux (`Linux`), selecting `gcc-16` on Darwin and `gcc` on Linux.

---

## Building & Running

All commands run from the repository root unless noted. Build output goes to `build/` (never into the source tree), except the demo binary.

### 1. Build
```bash
make                # the library only: build/lib/libcexpress.a
make demo           # library + demo app: examples/todo_sqlite/cexpress_demo
make clean          # remove build/ and the demo's objects and binary (cexpress_demo is tracked by git, so this shows as a deletion)
```
The Makefile tracks header dependencies, so editing a header rebuilds everything that includes it. The demo's own `Makefile` does not track the library: after changing `lib/`, rebuild it with `make -B -C examples/todo_sqlite`. Note that `examples/todo_sqlite/cexpress_demo` is committed to the repository: `make demo` rewrites it and `make clean` deletes it.

### 2. Run the server
The demo opens `public/index.html` and the default `todos.db` relative to its working directory, so start it from `examples/todo_sqlite/`:
```bash
cd examples/todo_sqlite
./cexpress_demo     # or: make run (inside examples/todo_sqlite/)
```
It listens on port `8080` and creates `todos.db` (SQLite) in the current directory (git-ignored). The demo serves its own web UI, so
open <http://localhost:8080/> in a browser, or check it from another terminal:
```bash
curl -i http://localhost:8080/api/todos       # 200 with a JSON array ([] on a fresh database)
```
Stop it with `Ctrl+C` (or `kill -TERM $(pgrep cexpress_demo)`): it stops accepting connections, finishes in-flight requests
and exits within 5 seconds. Common variants (all variables are listed under [Configuration](#configuration)):
```bash
PORT=3000 ./cexpress_demo                          # different port
WORKERS=4 QUIET=1 ./cexpress_demo                  # 4-process cluster, no per-request logging
WORKERS=auto ./cexpress_demo                       # one worker per CPU core
TODO_DB_PATH=/tmp/todos.db ./cexpress_demo         # database location
```
Started from any other directory, `GET /` answers 404 and the `/static` mount is not registered.
Write endpoints (`POST`/`PUT`/`PATCH`/`DELETE`) need `Authorization: Bearer <API_KEY>` (default `my-secret-api-key`);
see [Quick Start & Example Usage](#quick-start--example-usage) for `curl` examples.

### 3. Run with Docker
The multi-stage Alpine `Dockerfile` compiles the engine and the demo (`make all && make demo`) on Linux (musl, epoll). It does **not** run the test suite. The runtime image copies `cexpress_demo` and `public/` into `/app`:
```bash
docker build -t cexpress .
docker run --rm -p 8080:8080 cexpress
docker run --rm -p 8080:8080 -e WORKERS=4 -e API_KEY=change-me cexpress    # with options
```
The database lives inside the container and is discarded with it; mount a volume and set `TODO_DB_PATH` to keep it
(`-v cexpress-data:/data -e TODO_DB_PATH=/data/todos.db`). The server runs on epoll. `-e CEXPRESS_EVENT_LOOP=io_uring` selects io_uring instead, which Docker's default seccomp profile blocks (the server then exits at startup); add `--security-opt seccomp=unconfined --ulimit memlock=-1:-1` for it (see [concurrency.md](concurrency.md)).

---

## Running Tests

```bash
make test
```

builds and runs every suite (stops at the first failure; each prints `all ... tests passed`). Plain C `assert`
tests, no framework, no network access needed except loopback. All 13 suites passed on macOS (gcc-16) on 22 Sep 2026, plain and under ASan + UBSan; the Linux build was not run for this update. **13 suites:**

| Binary (`build/bin/`) | Covers |
|---|---|
| `test_middleware` | pipeline order, short-circuiting, 404/405/OPTIONS fallthrough, error handler |
| `test_router` | literal paths, `:params`, wildcards, sub-router mounting, route limits, `match_path` edge cases |
| `test_http_parser` | request line, headers, query, cookies, `Content-Length`, chunked bodies, keep-alive |
| `test_http_hardening` | regressions: header-name confusion, strict `Content-Length`, method limits, garbage-filled `Request` |
| `test_connection` | non-blocking socket I/O over `socketpair(2)`: partial reads, buffer growth, 400/413/414/431, timeouts |
| `test_response` | headers, cookies, redirects, chunked streaming, trailers, HEAD, header/cookie injection refusal |
| `test_multipart` / `test_urlencoded` | form and file-upload body parsers |
| `test_static` | path traversal and symlink escape prevention, MIME types, directory index |
| `test_event_loop` | event-loop lifecycle, readiness polling, idle/shutdown timers |
| `test_cluster` | worker count, `SO_REUSEPORT` multi-bind, concurrent serving, graceful drain |
| `test_ping` | a minimal `/ping` route through parse → route → dispatch (the connection stress-test target's shape) |
| `test_cookbook` | every recipe in `lib/examples/cookbook.c`, driven through the real parse → route → dispatch path (this is also the only automated JSON coverage) |

### Run one suite
```bash
make build/bin/test_router && ./build/bin/test_router
```
Substitute any name from the table. A failing `assert` prints the file, line and expression.

### Memory-safety checks
```bash
make SANITIZE=1 BUILD_DIR=build-asan test     # every suite under AddressSanitizer + UBSan
make fuzz                                     # mutation fuzzer over parser/router/response (FUZZ_ITERS=n, default 1,000,000)
```
Use a separate `BUILD_DIR` for sanitizer builds so instrumented and normal objects never mix. Leak detection is on by
default on Linux; AddressSanitizer on macOS cannot detect leaks (and `tests/test_router.c` does not free its route trees, so a Linux leak run would report them). Run both after touching `lib/http_parser.c`,
`lib/router.c`, `lib/response.c` or `lib/arena.c`. The fuzzer checks memory safety only; it does not check that malformed requests are answered.

### Performance
```bash
make bench             # CPU cost per request (no sockets) and yyjson list emission, in ns
scripts/stress_test.sh # end-to-end wrk sweep against a real cluster (needs wrk); read scripts/CLAUDE.md first
PHASES=ping scripts/stress_test.sh   # only the DB-free /ping connection benchmark (PHASES: ping churn read write)
```

### Documentation check
```bash
make check-docs        # fails if lib/API.md and the lib/*.h headers disagree about the public functions
```

### Linux from a Mac
The Docker build compiles the Linux engine but does not run the tests. To run `make test` on Linux use a Linux host or container with `gcc`, `make`, `liburing-dev` and `sqlite-dev` (the builder stage of the `Dockerfile` lists the packages); this was not done for this update. On macOS, `make test_epoll` additionally exercises the epoll backend through `epoll-shim` when it is installed (`brew install epoll-shim`).

---

## Configuration

The demo app supports runtime environment variables; the engine itself is configured in code (`app.config`, ...):

| Environment Variable | Default Value | Description |
|---|---|---|
| `PORT` | `8080` | TCP port the server binds to (valid range: `1`–`65535`). |
| `WORKERS` | `1` | Number of worker processes (`1` = single process, `auto` or `0` = CPU core auto-detection, `N` = fixed count, at most 128). |
| `API_KEY` | `my-secret-api-key` | Bearer token verified by the demo authentication middleware. |
| `TODO_DB_PATH` | `todos.db` | Path to the SQLite database file backing the Todo CRUD demo. |
| `QUIET` | unset | `1` disables the per-request access log (use it for benchmarks). |

### Running with Custom Configuration
```bash
cd examples/todo_sqlite
PORT=3000 API_KEY=super-secret-token ./cexpress_demo
```

### Engine Limits (`lib/app_types.h`)
- `BUF_SIZE`: Initial input buffer per connection and the request-header limit (default: `8192` bytes).
- `MAX_BODY_SIZE`: Maximum request body and streamed response size (default: `10MB`).
- `MAX_ROUTER_ROUTES`: Routes per `Router` before it is mounted (default: `64`); an `App` has no fixed route cap.
- `MAX_MIDDLEWARES`: Maximum app-wide middlewares (default: `16`); `MAX_ROUTE_MIDDLEWARES`: per route (default: `8`).
- `MAX_PARAMS`: Maximum path parameters captured per route (default: `8`).
- `MAX_HEADERS`: Request headers kept; a 33rd header is a `400` (default: `32`).
- `INITIAL_CONNECTION_TABLE_CAP`: Starting size of the connection table (default: `1024`, grows dynamically).
- Per-connection arena: 64 KiB (`ARENA_SIZE` in `lib/connection.c`).

---

## Quick Start & Example Usage

Start the server:
```bash
cd examples/todo_sqlite
PORT=8080 ./cexpress_demo
```

The demo app (`examples/todo_sqlite/`) is a SQLite-backed Todo CRUD API — see
[`examples/todo_sqlite/CLAUDE.md`](examples/todo_sqlite/CLAUDE.md) for its architecture. Open
`http://localhost:8080/` in a browser for the built-in Todo UI, or drive the
REST API directly:

### 0. Ping (GET)
```bash
curl -i http://localhost:8080/ping       # 200, body "pong": no database, no JSON
```
The connection stress-test target: `PHASES=ping scripts/stress_test.sh` (keep-alive throughput) and
`PHASES=churn scripts/stress_test.sh` (a new TCP connection per request). See [`scripts/CLAUDE.md`](scripts/CLAUDE.md).

### 1. Todo UI (GET)
```bash
curl -i http://localhost:8080/
```
A single self-contained page (`examples/todo_sqlite/public/index.html`) served via bounded
file streaming (`res_send_file`) — list/add/toggle/delete todos, with an
API-key field for the protected routes below.

### 2. List Todos, Optionally Filtered (GET)
```bash
curl -i http://localhost:8080/api/todos
curl -i "http://localhost:8080/api/todos?done=true"
```

### 3. Create a Todo (POST, protected)
```bash
curl -i -X POST \
  -H "Authorization: Bearer my-secret-api-key" \
  -H "Content-Type: application/json" \
  -d '{"title":"Buy milk"}' \
  http://localhost:8080/api/todos
```

### 4. Get a Todo by ID (GET)
```bash
curl -i http://localhost:8080/api/todos/1
```

### 5. Replace a Todo (PUT, protected)
```bash
curl -i -X PUT \
  -H "Authorization: Bearer my-secret-api-key" \
  -H "Content-Type: application/json" \
  -d '{"title":"Buy oat milk","done":false}' \
  http://localhost:8080/api/todos/1
```

### 6. Partially Update a Todo (PATCH, protected)
```bash
curl -i -X PATCH \
  -H "Authorization: Bearer my-secret-api-key" \
  -H "Content-Type: application/json" \
  -d '{"done":true}' \
  http://localhost:8080/api/todos/1
```

### 7. Delete a Todo (DELETE, protected)
```bash
curl -i -X DELETE -H "Authorization: Bearer my-secret-api-key" http://localhost:8080/api/todos/1
```

### 8. Graceful Shutdown
Send `SIGINT` (`Ctrl+C`) or `SIGTERM` to the server process:
```bash
kill -TERM $(pgrep cexpress_demo)
```
The server stops accepting new connections, finishes in-flight requests, and shuts down cleanly within 5 seconds.

---

## Project Layout

```
.
├── Makefile              # OS-detecting build rules: library, tests, bench, fuzz, docs check
├── Dockerfile            # Multi-stage Alpine build of the library and demo (does not run tests)
├── .dockerignore         # Build context exclusions
├── LICENSE
├── README.md             # Project documentation (this file)
├── DOC.md                # API guide: routing, middleware, request/response, memory model, JSON
├── importing.md          # Using CExpress from another project (tested Makefile + main.c)
├── concurrency.md        # Multi-worker concurrency & SO_REUSEPORT architecture guide
├── tradeoffs.md          # Arena, yyjson, picohttpparser, Patricia router, io_uring: costs and measurements
├── todo.md               # Cheat sheet for running the demo (not a task list)
├── finds.md              # Point-in-time review notes on the docs (2026-09-21)
├── improvements.md       # Prioritized list of speed, security and memory fixes, with measured or estimated gains
├── CLAUDE.md             # Project standards, coding guidelines, and workflow rules (AGENTS.md is a symlink to it)
├── docs/                 # index.html: static documentation and benchmark site
├── lib/                  # Reusable CExpress engine (builds to build/lib/libcexpress.a)
│   ├── CLAUDE.md         # Engine map: lifecycle, memory model, limits, ownership, hot-path rules, known gaps
│   ├── API.md            # One-line index of every public function (checked by `make check-docs`)
│   ├── examples/         # cookbook.c: tested recipes (JSON, params, middleware, cookies, uploads, streaming)
│   ├── cexpress.h        # Umbrella header (includes yyjson)
│   ├── app_types.h       # Struct definitions, event loop types, function pointer signatures, limits
│   ├── arena.h/c         # Per-connection bump allocator (+ yyjson allocator adapter)
│   ├── cluster.h/c       # Multi-process master supervisor & worker lifecycle
│   ├── event_loop.h      # Cross-platform event-loop abstraction
│   ├── event_loop_kqueue.c   # Native BSD/macOS kqueue backend
│   ├── event_loop_io_uring.c # Linux io_uring readiness backend (liburing; timerfd + signalfd)
│   ├── event_loop_epoll.c    # epoll backend, compiled with -DCEXPRESS_USE_EPOLL
│   ├── connection.h/c    # Non-blocking socket I/O, buffers, arena lifetime & graceful shutdown
│   ├── http_parser.h/c   # Request parsing on picohttpparser, framing, query string, chunked decoding
│   ├── router.h/c        # Route registration, per-method Patricia trees, sub-routers
│   ├── response.h/c      # Response builder, streaming chunks & trailers, file streaming
│   ├── middleware.h/c    # MiddlewareChain and dispatch pipeline
│   ├── static.h/c        # Static file serving with path-traversal guards
│   ├── multipart.h/c     # RFC 7578 multipart/form-data parser
│   ├── urlencoded.h/c    # application/x-www-form-urlencoded parser
│   └── vendor/           # Vendored third-party source
│       ├── picohttpparser/   # HTTP request tokenizer
│       └── yyjson/           # JSON reader/writer (0.13.0)
├── examples/todo_sqlite/ # Reference demo application: SQLite-backed Todo CRUD
│   ├── CLAUDE.md         # Application wiring, persistence layer & fork-safety notes
│   ├── Makefile          # Builds cexpress_demo against ../../build/lib/libcexpress.a
│   ├── main.c            # Application entrypoint, route definitions, inline /ping handler
│   ├── handlers.h/c      # Todo CRUD route handlers (yyjson over the connection arena)
│   ├── middlewares.h/c   # Logger, body size guard, authentication and JSON error middlewares
│   ├── db.h/c            # SQLite persistence layer (schema, CRUD, worker-init hook)
│   ├── todo_types.h      # Todo/TodoList struct definitions
│   └── public/           # Static asset directory (served via app_serve_static)
│       ├── index.html    # Self-contained Todo UI (served directly at GET /)
│       ├── style.css     # Static-file-serving demo asset
│       └── docs/         # Directory-index fallback demo
├── tests/                # Isolated test suites (built into build/bin/test_*)
│   ├── CLAUDE.md         # Test harness architecture, arena usage and socket mocking strategy
│   ├── test_connection.c # Socket I/O and lifecycle tests via socketpair(2)
│   ├── test_event_loop.c # Event-loop abstraction tests (lifecycle, I/O, timers)
│   ├── test_cluster.c    # Multi-worker cluster tests (forking, SO_REUSEPORT, drain)
│   ├── test_http_parser.c# Unit tests for HTTP parser and request dechunking
│   ├── test_http_hardening.c # Regression tests for parser bugs (framing, method limits, ...)
│   ├── test_ping.c       # A minimal /ping route through parse -> route -> dispatch
│   ├── test_cookbook.c   # Runs every cookbook recipe through parse -> route -> dispatch
│   ├── bench_hotpath.c   # `make bench`: per-request CPU cost, yyjson list emission
│   ├── fuzz_parser.c     # `make fuzz`: mutation fuzzer under ASan + UBSan
│   ├── test_middleware.c # Middleware chain dispatching and error handling
│   ├── test_router.c     # Unit tests for router pattern matching and sub-routers
│   ├── test_response.c   # Unit tests for headers, cookies, redirects, chunking & trailers
│   ├── test_multipart.c  # Unit tests for multipart/form-data parser
│   ├── test_urlencoded.c # Unit tests for urlencoded parser
│   └── test_static.c     # Unit tests for static file serving and traversal guards
├── scripts/               # Benchmarking and utility scripts
│   ├── CLAUDE.md          # Benchmarking tools documentation and known problems
│   ├── stress_test.sh     # End-to-end wrk benchmark: build, boot cluster, seed, run (PHASES=ping|churn|read|write)
│   ├── docker_stress_test.sh # Same idea inside Docker, to exercise the Linux io_uring backend
│   ├── check_docs.sh      # `make check-docs`: keeps lib/API.md in sync with the headers
│   ├── export_framework.sh # `make export DEST=...`: copies the engine into another project
│   └── wrk_create_todo.lua # wrk load-testing script for POST /api/todos
├── stress_tests/          # Recorded stress run
│   ├── stress_test_report.md   # Report: current measurements and earlier results
│   └── full_run_output.txt     # Raw output of one scripts/stress_test.sh run (2026-09-21, see its caveats)
└── build/                # Out-of-source build outputs (gitignored)
    ├── bin/              # test runners, bench and fuzz binaries
    ├── lib/              # libcexpress.a static library
    └── obj/              # Object files (*.o)
```

---

## Roadmap

All 11 original architectural milestones have been completed:
- [x] Sub-routers & prefix mounting (`app_mount`, `Router`)
- [x] Additional HTTP verbs (`PUT`, `DELETE`, `PATCH`, `OPTIONS`, `HEAD`)
- [x] Query string parser (`req_get_query`) & URL percent-decoding (`url_decode`)
- [x] Request body dechunking (`Transfer-Encoding: chunked`)
- [x] Static file serving (`app_serve_static`)
- [x] Cookie helpers (`res_set_cookie`, `res_clear_cookie`, `req_get_cookie`)
- [x] Graceful shutdown on `SIGINT`/`SIGTERM` with in-flight request draining
- [x] Streaming & chunked responses (`res_write`, `res_end`, `res_set_trailer`, `res_send_file`)
- [x] Cross-platform event backend (`kqueue` for macOS/BSD, `epoll` for Linux, `io_uring` opt-in)
- [x] Multi-process worker model (`SO_REUSEPORT`)

Engine changes since then (see [`tradeoffs.md`](tradeoffs.md)):
- [x] yyjson replaces the in-house JSON library
- [x] Per-connection arena allocator
- [x] picohttpparser replaces the handwritten request parser
- [x] Patricia-tree router (no fixed route cap)
- [x] io_uring event loop on Linux (opt-in since 2026-09-24; epoll is the default)
- [x] TLS / HTTPS support (OpenSSL/LibreSSL non-blocking handshake integration) added, then removed (2026-09-22): TLS termination belongs at a gateway or reverse proxy in front of this engine, not inside a library that only parses HTTP/1.1

Open items are listed under [Known Gaps](#known-gaps).
