# CExpress

A lightweight, high-performance, single-threaded HTTP/1.1 server and web framework written entirely in C (C11), designed with a developer experience inspired by [Express.js](https://expressjs.com/).

> [!NOTE]
> **Library Architecture:**
> The code in `lib/` compiles to a reusable static library (`build/lib/libcexpress.a`). The code in `examples/todo_sqlite/` is a reference implementation — a SQLite-backed Todo CRUD API — showing how to consume the library. See [`examples/todo_sqlite/CLAUDE.md`](examples/todo_sqlite/CLAUDE.md) for its architecture.

---

## Table of Contents
- [Purpose](#purpose)
- [Performance & Benchmarks](#performance--benchmarks)
- [Architecture Overview](#architecture-overview)
- [Features](#features)
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
- **Maximum Performance & Low Latency**: Native execution with minimal CPU overhead, sub-millisecond response times, and about 287,000 requests/sec on a minimal endpoint with 4 workers (about 207,000 req/sec with a single process). See [Performance & Benchmarks](#performance--benchmarks).
- **Minimal Footprint**: `lib/` itself has zero external dependencies beyond standard C and POSIX APIs (optional OpenSSL for TLS). The bundled demo app additionally links SQLite (embedded, no server process) for its Todo persistence layer.
- **Event-Driven Non-Blocking I/O**: High-performance concurrency powered by native `kqueue` (macOS / BSD) and `epoll` (Linux), following the same architectural pattern as Node.js's underlying `libuv`.
- **Memory Safety & Control**: Explicit bounded buffers, aggressive `const` correctness, bounded I/O guards, and strict dynamic memory allocation tracking.

---

## Performance & Benchmarks

Measured with `wrk` (8 threads, keep-alive, 15 s per row) against the server started exactly as
`QUIET=1 WORKERS=4 ./build/bin/cexpress`, on an Apple M3 Pro laptop with `wrk` running on the same machine
(so both compete for the same cores). Each row is a single run; repeated runs of the same command varied by about ±1%
(`/ping`, 100 connections: 289k, 285k, 286k, 287k req/s).

**`GET /ping`**: a fixed 4-byte reply, no database, no JSON. This is the engine's connection and request path on its own.

| Concurrency | Throughput | Avg Latency | Max Latency | Total Requests | Data Transferred |
|---|---|---|---|---|---|
| **100 connections** | **286,886 req/sec** | **330 µs** | 4.29 ms | 4,331,986 | 380.08 MB |
| **1,000 connections** | **286,401 req/sec** | **3.47 ms** | 11.70 ms | 4,299,189 | 377.20 MB |
| **5,000 connections** | **253,306 req/sec** | **17.34 ms** | 51.26 ms | 3,811,353 | 334.40 MB |

**`GET /`**: the Todo UI, a 6,481-byte HTML page streamed from disk (`res_send_file`) on every request.

| Concurrency | Throughput | Avg Latency | Max Latency | Total Requests | Data Transferred |
|---|---|---|---|---|---|
| **100 connections** | **69,782 req/sec** | **1.37 ms** | 4.72 ms | 1,053,741 | 6.45 GB |
| **1,000 connections** | **66,054 req/sec** | **15.07 ms** | 28.52 ms | 991,471 | 6.07 GB |
| **5,000 connections** | **64,172 req/sec** | **52.36 ms** | 133.37 ms | 965,895 | 5.91 GB |

At 5,000 connections `wrk` also reported read errors (636 on `/ping`, 2,051 on `GET /`; no connect errors, no worker
crashes); the cause is not identified. With `WORKERS=1` the same test gives 207,026 req/sec on `/ping` (100
connections) but 70,119 on `GET /`, the same as with 4 workers, so the Todo UI page is not limited by the server's CPU
count; the reason has not been isolated. Earlier versions of this table (about 202k req/sec) were taken when `GET /`
returned a 26-byte in-memory string, so they compare with `/ping`, not with today's `GET /`.

> [!TIP]
> Reproduce the tables above manually (this is the setup they were measured with):
> ```bash
> # Terminal 1: 4-worker cluster, no access logging
> QUIET=1 WORKERS=4 ./build/bin/cexpress
>
> # Terminal 2:
> wrk -t8 -c100  -d15s http://127.0.0.1:8080/ping
> wrk -t8 -c1000 -d15s http://127.0.0.1:8080/ping
> wrk -t8 -c5000 -d15s http://127.0.0.1:8080/ping
> wrk -t8 -c100  -d15s http://127.0.0.1:8080/        # Todo UI
> wrk -t8 -c100  -d15s http://127.0.0.1:8080/api/todos
> ```
> `scripts/stress_test.sh` automates a full sweep (builds, boots a 4-worker cluster, seeds todos, runs `GET /ping`,
> connection churn, `GET /`, `GET /api/todos` and `POST /api/todos`, and tracks memory), e.g.
> `WORKERS=8 CONNS="100 1000 5000 10000" scripts/stress_test.sh` — see [`scripts/CLAUDE.md`](scripts/CLAUDE.md).
> Its numbers run lower than the manual ones on this machine (about 200–209k req/sec on `/ping` at 100 connections in the same
> session; the memory sampler accounts for only about 4% of that), so compare figures only within the same method.

---

## Architecture Overview

The codebase is split into two distinct tiers:

```
┌────────────────────────────────────────────────────────┐
│        examples/todo_sqlite/ (Demo App: SQLite-backed Todo CRUD)        │
│   - main.c (Configures routes, routers & boots server) │
│   - handlers.c (Todo CRUD route handlers)              │
│   - middlewares.c (Auth, logging, body size guards)    │
│   - db.c (SQLite schema, CRUD, worker-init hook)       │
└───────────────────────────┬────────────────────────────┘
                            │ links against
┌───────────────────────────▼────────────────────────────┐
│              lib/ (Core Engine - libcexpress.a)        │
│   - connection.c : Portable socket I/O & lifecycle     │
│   - cluster.c/h  : Multi-worker SO_REUSEPORT supervisor│
│   - event_loop.h : Cross-platform event-loop interface │
│   - event_loop_kqueue.c: Native macOS/BSD kqueue loop  │
│   - event_loop_epoll.c : Native Linux epoll loop       │
│   - http_parser.c: HTTP/1.1 parser, query & dechunking │
│   - router.c     : Pattern matcher, sub-routers, mount │
│   - middleware.c : Dispatch & middleware pipeline      │
│   - response.c   : Response builder, chunks & trailers │
│   - static.c     : Traversal-safe static file serving  │
│   - multipart.c  : RFC 7578 multipart/form-data parser│
│   - urlencoded.c : application/x-www-form-urlencoded   │
│   - json/        : JSON parser, AST & streaming writer │
└────────────────────────────────────────────────────────┘
```

---

## Features

### 1. Express-Style Routing & Sub-Routers
- **Full HTTP Method Suite**: `app_get()`, `app_post()`, `app_put()`, `app_patch()`, `app_delete()`, `HEAD` (RFC 7230 §3.3.3 body suppression), and automatic `OPTIONS` (`Allow:` header generation).
- **Path Parameter Extraction**: Named segment variables (e.g. `/users/:id`, `/orgs/:orgId/repos/:repoName`) accessible via `req_get_param(req, "id")`.
- **Wildcard Matching**: Trailing wildcards (e.g. `/files/*`).
- **Sub-Routers & Prefix Mounting**: Standalone `Router` instances mounted via `app_mount(&app, "/api", &router)` with nested middleware scoping.
- **Deny-by-Default**: Unmatched routes return `404 Not Found`; mismatched methods return `405 Method Not Allowed` with valid `Allow` headers.

### 2. Middleware & Centralized Error Pipeline
- **Sequential Execution**: Middleware functions execute in order with `chain_next(chain)`.
- **Prefix-Scoped Middleware**: `app_use_prefix(app, "/api", mw)` gates entire URL hierarchies.
- **Per-Route Middleware**: `app_get_mw(...)` and `app_post_mw(...)` attach guards to specific routes.
- **Short-Circuiting & Post-Processing**: Early return on failure (e.g. 401/403) or inspection after the handler finishes (e.g. access logging).
- **Centralized Error Handling**: `chain_error(chain, status, message)` mirrors Express's `(err, req, res, next)`.
- **Built-in Demo Middlewares**: `mw_logger`, `mw_body_size_guard`, `mw_authenticate` (constant-time token verification), and `error_handler_json`.

### 3. Non-Blocking Event-Driven Networking & Multi-Worker Concurrency
- **Multi-Worker Process Model (`SO_REUSEPORT`)**: Scale across all CPU cores with zero lock contention. Dedicated listening sockets per worker with kernel-level TCP connection distribution (see [concurrency.md](concurrency.md)).
- **Master Process Supervision**: Automatically reaps dead children, prevents container zombie leaks, respawns crashed workers on the fly, and coordinates clean graceful drains.
- **Cross-Platform Event Loop**: Native `kqueue` on macOS/BSD and native `epoll` (`epoll_create1`, `timerfd`, `signalfd`) on Linux with zero external dependencies.
- **Dynamic Connection Table**: Bounded only by `RLIMIT_NOFILE`, growing dynamically via `ensure_connection_capacity`.
- **HTTP/1.1 Keep-Alive**: Persistent connections with idle connection timeout sweeps.
- **Graceful Shutdown**: Synchronous signal trapping (`SIGINT`/`SIGTERM`), stops accepting new connections, drains in-flight responses, and enforces a 5-second deadline timer before clean exit.

### 4. Robust HTTP/1.1 Parser & Security Guards
- **Zero-Copy & Pure Parsing**: Side-effect-free parser operating on read-only byte buffers.
- **Chunked Request Dechunking**: Decodes incoming `Transfer-Encoding: chunked` payloads, handling chunk extensions, trailers, and rejecting smuggling attacks (400 if both `Transfer-Encoding` and `Content-Length` are sent).
- **Query String & URL Decoding**: Fast query string extraction (`req_get_query`) and percent-decoding (`url_decode`).
- **Security Limits**: Rejects oversized/negative `Content-Length` (400), payload overflow (413), and header overflow / Slowloris patterns (431).

### 5. Streaming & Chunked Responses
- **Procedural Chunked Response API**: `res_write(res, data, len)` and `res_end(res)` emit HTTP/1.1 chunked framing (`<hex>\r\n<data>\r\n`) with dynamic buffer growth up to 10MB while preserving pure handler testability.
- **RFC 7230 Chunked Trailers**: `res_set_trailer(res, name, value)` declares `Trailer:` in headers and outputs trailers after the terminal `0\r\n` chunk.
- **Bounded 16KB File Streaming**: `res_send_file(res, content_type, filepath)` streams files in 16KB chunks directly through `flush_connection` with 64KB cooperative yielding per event-loop turn, never buffering whole files into RAM.

### 6. Static File Serving
- **Mounted Route Serving**: `app_serve_static(app, "/static", "examples/todo_sqlite/public")`.
- **Path-Traversal Protection**: Pure textual `..` segment rejection paired with realpath canonical containment checks against symlink escapes.
- **Directory Index Fallback**: Automatically serves `index.html` for directory requests.
- **Built-in MIME Registry**: Automatic Content-Type resolution for CSS, JS, HTML, PNG, JPEG, SVG, JSON, and binaries.

### 7. Form & Body Parsers
- **Multipart Form Data**: RFC 7578 parser (`lib/multipart.h`) extracting fields and binary file parts with embedded NUL safety.
- **URL-Encoded Forms**: `application/x-www-form-urlencoded` parser (`lib/urlencoded.h`).

### 8. Cookie Management
- **Cookie Parsing**: Access request cookies via `req_get_cookie(req, "session")`.
- **Cookie Setting & Clearing**: `res_set_cookie(res, name, val, &opts)` and `res_clear_cookie(res, name, path)` supporting `Path`, `Domain`, `Max-Age`, `HttpOnly`, `Secure`, and `SameSite` (`Strict`/`Lax`/`None`).

### 9. Built-in JSON Parser & Writer
- Pure C recursive-descent JSON parser in `lib/json/`: `json_parse` builds a tree (`null`, booleans, numbers, escaped strings, arrays, objects) that you read with `json_object_get` / `json_as_string` / ... and release with `json_free`.
- **`JsonWriter`** (`jw_object_begin`, `jw_key`, `jw_int`, `jw_string`, ...) emits JSON straight into a buffer: no tree, automatic commas and string escaping, exact 64-bit integers, and one `jw_ok` check at the end. About 4x faster than building a tree; the demo app uses it for every response.
- A tree builder API (`json_new_*`, `json_object_set`, `json_array_append`) and `json_stringify` remain for editing or forwarding parsed documents.

---

## Prerequisites

- **Operating System**: macOS / BSD (native `kqueue`), Linux (native `epoll`), or Docker on any host.
- **Compiler**: C11 compliant compiler (`gcc-16` on macOS; `gcc` on Linux). The `Makefile` picks one by OS; override with `make CC=<compiler>`.
- **Build Tool**: GNU `make`.
- **SQLite development headers**: required to build the demo app (`examples/todo_sqlite/db.c`) — `sqlite-dev` on Alpine, `libsqlite3-dev` on Debian/Ubuntu, or `brew install sqlite` on macOS. The `Makefile` auto-detects a Homebrew keg, falls back to `pkg-config`, then a bare `-lsqlite3`.
- **OpenSSL development headers** (optional): enables HTTPS. Auto-detected; if absent, or with `make NO_TLS=1`, the server builds without TLS.
- **Optional**: Docker (containerized run; its build also runs the whole test suite on Linux), and [`wrk`](https://github.com/wg/wrk) for load testing (`brew install wrk` / `apt install wrk`).

> [!TIP]
> On macOS, `gcc-16` is installed via Homebrew (`brew install gcc`). The default `Makefile` automatically detects macOS (`Darwin`) or Linux (`Linux`), selecting `gcc-16` on Darwin and `gcc` on Linux.

---

## Building & Running

All commands run from the repository root. Build output goes to `build/` (never into the source tree).

### 1. Build
```bash
make                # library + demo app
make NO_TLS=1       # same, without OpenSSL (plaintext only)
make clean          # remove build/ and the ./cexpress symlink
```
This produces:
- `build/lib/libcexpress.a`: the core engine static library.
- `build/bin/cexpress`: the demo application (also symlinked to `./cexpress`).

The Makefile tracks header dependencies, so editing a header rebuilds everything that includes it.

### 2. Run the server
```bash
./cexpress          # or: make run
```
It listens on port `8080` and creates `todos.db` (SQLite) in the current directory. The demo serves its own web UI, so
open <http://localhost:8080/> in a browser, or check it from another terminal:
```bash
curl -i http://localhost:8080/api/todos       # 200 with a JSON array ([] on a fresh database)
```
Stop it with `Ctrl+C` (or `kill -TERM $(pgrep cexpress)`): it stops accepting connections, finishes in-flight requests
and exits within 5 seconds. Common variants (all variables are listed under [Configuration](#configuration)):
```bash
PORT=3000 ./cexpress                          # different port
WORKERS=4 QUIET=1 ./cexpress                  # 4-process cluster, no per-request logging
WORKERS=auto ./cexpress                       # one worker per CPU core
TODO_DB_PATH=/tmp/todos.db ./cexpress         # database location
TLS_CERT=tests/certs/server.crt TLS_KEY=tests/certs/server.key PORT=8443 ./cexpress
curl -k https://localhost:8443/api/todos      # -k: the bundled test certificate is self-signed
```
Write endpoints (`POST`/`PUT`/`PATCH`/`DELETE`) need `Authorization: Bearer <API_KEY>` (default `my-secret-api-key`);
see [Quick Start & Example Usage](#quick-start--example-usage) for `curl` examples.

### 3. Run with Docker
The multi-stage Alpine `Dockerfile` compiles the engine and **runs the full test suite during `docker build`**, so a
successful build means the tests passed on Linux (`epoll`, musl):
```bash
docker build -t cexpress .
docker run --rm -p 8080:8080 cexpress
docker run --rm -p 8080:8080 -e WORKERS=4 -e API_KEY=change-me cexpress    # with options
```
The database lives inside the container and is discarded with it; mount a volume and set `TODO_DB_PATH` to keep it
(`-v cexpress-data:/data -e TODO_DB_PATH=/data/todos.db`).

---

## Running Tests

```bash
make test
```

builds and runs every suite (stops at the first failure; each prints `all ... tests passed`). Plain C `assert`
tests, no framework, no network access needed except loopback. **15 suites:**

| Binary (`build/bin/`) | Covers |
|---|---|
| `test_json` | JSON parsing, tree builders, `json_stringify` number formatting, the streaming `JsonWriter` |
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
| `test_tls` | non-blocking TLS handshake and I/O (uses `tests/certs/`) |
| `test_ping` | the demo app's `GET /ping` handler (the connection stress-test target) |
| `test_cookbook` | every recipe in `lib/examples/cookbook.c`, driven through the real parse → route → dispatch path |

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
default on Linux; AddressSanitizer on macOS cannot detect leaks. Run both after touching `lib/http_parser.c`,
`lib/router.c`, `lib/response.c` or `lib/json/json_writer.c`.

### Performance
```bash
make bench             # CPU cost per request (no sockets) and JSON writer vs tree, in ns
scripts/stress_test.sh # end-to-end wrk sweep against a real cluster (needs wrk); see scripts/CLAUDE.md
PHASES=ping scripts/stress_test.sh   # only the DB-free /ping connection benchmark (PHASES: ping churn read write)
```

### Documentation check
```bash
make check-docs        # fails if lib/API.md and the lib/ headers disagree about the public functions
```

### Linux from a Mac
`docker build -t cexpress .` runs the whole suite on Linux (see above). On macOS, `make test_epoll` additionally exercises
the epoll backend through `epoll-shim` when it is installed (`brew install epoll-shim`).

---

## Configuration

The server supports both runtime environment variables and programmatic configuration:

| Environment Variable | Default Value | Description |
|---|---|---|
| `PORT` | `8080` | TCP port the server binds to (valid range: `1`–`65535`). |
| `WORKERS` | `1` | Number of worker processes (`1` = single process, `auto` or `0` = CPU core auto-detection, `N` = fixed count). |
| `API_KEY` | `my-secret-api-key` | Bearer token verified by the demo authentication middleware. |
| `TODO_DB_PATH` | `todos.db` | Path to the SQLite database file backing the Todo CRUD demo. |
| `QUIET` | unset | `1` disables the per-request access log (use it for benchmarks). |
| `TLS_CERT` / `TLS_KEY` | unset | PEM certificate chain and private key; set **both** to serve HTTPS. |

### Running with Custom Configuration
```bash
PORT=3000 API_KEY=super-secret-token ./cexpress
```
Environment variables are read by the demo app (`examples/todo_sqlite/main.c`); the engine itself is configured in code (`app.config`,
`app_enable_tls`, ...).

### Engine Limits (`lib/app_types.h`)
- `BUF_SIZE`: Initial buffer per connection (default: `8192` bytes).
- `MAX_BODY_SIZE`: Maximum request/response body size (default: `10MB`).
- `MAX_ROUTES`: Maximum registered routes per router (default: `32`).
- `MAX_MIDDLEWARES`: Maximum app-wide middlewares (default: `16`).
- `MAX_PARAMS`: Maximum path parameters captured per route (default: `8`).
- `INITIAL_CONNECTION_TABLE_CAP`: Starting size of the connection table (default: `1024`, grows dynamically).

---

## Quick Start & Example Usage

Start the server:
```bash
PORT=8080 ./cexpress
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
kill -TERM $(pgrep cexpress)
```
The server stops accepting new connections, finishes in-flight requests, and shuts down cleanly within 5 seconds.

---

## Project Layout

```
.
├── Makefile              # OS-detecting build rules for macOS (kqueue) & Linux (epoll)
├── Dockerfile            # Multi-stage container build and test harness
├── .dockerignore         # Build context exclusions
├── README.md             # Project documentation
├── concurrency.md        # Multi-worker concurrency & SO_REUSEPORT architecture guide
├── CLAUDE.md             # Project standards, coding guidelines, and workflow rules
├── AGENTS.md             # Agent context and workflow guidelines
├── lib/                  # Reusable CExpress engine (builds to build/lib/libcexpress.a)
│   ├── CLAUDE.md         # Engine map: lifecycle, limits, ownership, hot-path rules, known gaps
│   ├── API.md            # One-line index of every public function (checked by `make check-docs`)
│   ├── examples/         # cookbook.c: tested recipes (JSON, params, middleware, cookies, uploads, streaming)
│   ├── app_types.h       # Struct definitions, event loop types, function pointer signatures
│   ├── cluster.h/c       # Multi-process master supervisor & worker lifecycle
│   ├── event_loop.h      # Cross-platform event-loop abstraction
│   ├── event_loop_kqueue.c # Native BSD/macOS kqueue backend
│   ├── event_loop_epoll.c  # Native Linux epoll backend (timerfd + signalfd)
│   ├── connection.h/c    # Portable non-blocking socket I/O & graceful shutdown
│   ├── tls.h/c           # OpenSSL/LibreSSL non-blocking TLS lifecycle & fallback
│   ├── http_parser.h/c   # HTTP/1.1 parser, query string, chunked decoding
│   ├── router.h/c        # Path pattern matching, route table, sub-routers
│   ├── response.h/c      # Response builder, streaming chunks & trailers, file streaming
│   ├── middleware.h/c    # MiddlewareChain and dispatch pipeline
│   ├── static.h/c        # Static file serving with path-traversal guards
│   ├── multipart.h/c     # RFC 7578 multipart/form-data parser
│   ├── urlencoded.h/c    # application/x-www-form-urlencoded parser
│   └── json/             # JSON parser, tree API and streaming JsonWriter
│       ├── CLAUDE.md     # JSON subsystem architecture and memory rules
│       ├── json.h        # Public JSON API
│       ├── json_types.h  # AST enum and node structs
│       └── json_*.c      # Parser, value accessors, and stringifier
├── examples/todo_sqlite/                  # Reference demo application: SQLite-backed Todo CRUD
│   ├── CLAUDE.md         # Application wiring, persistence layer & fork-safety notes
│   ├── main.c            # Application entrypoint and route definitions
│   ├── handlers.h/c      # Todo CRUD route handlers
│   ├── ping.h/c          # GET /ping handler (connection stress-test target, no DB)
│   ├── middlewares.h/c   # Logger, body size guard, and authentication middlewares
│   ├── db.h/c            # SQLite persistence layer (schema, CRUD, worker-init hook)
│   ├── todo_types.h      # Todo/TodoList struct definitions
│   └── public/           # Static asset directory (served via app_serve_static)
│       ├── index.html    # Self-contained Todo UI (served directly at GET /)
│       ├── style.css     # Static-file-serving demo asset
│       └── docs/         # Directory-index fallback demo
├── tests/                # Isolated test suites (built into build/bin/test_*)
│   ├── CLAUDE.md         # Test harness architecture and socket mocking strategy
│   ├── certs/            # RSA test certificates for TLS verification
│   ├── test_connection.c # Socket I/O and lifecycle tests via socketpair(2)
│   ├── test_tls.c        # Non-blocking TLS handshake, I/O & session tests
│   ├── test_event_loop.c # Event-loop abstraction tests (lifecycle, I/O, timers)
│   ├── test_cluster.c    # Multi-worker cluster tests (forking, SO_REUSEPORT, drain)
│   ├── test_http_parser.c# Unit tests for HTTP parser and request dechunking
│   ├── test_http_hardening.c # Regression tests for parser bugs (framing, method limits, ...)
│   ├── test_ping.c       # GET /ping handler tests
│   ├── test_cookbook.c   # Runs every cookbook recipe through parse -> route -> dispatch
│   ├── bench_hotpath.c   # `make bench`: per-request CPU cost, JSON writer vs tree
│   ├── fuzz_parser.c     # `make fuzz`: mutation fuzzer under ASan + UBSan
│   ├── test_middleware.c # Middleware chain dispatching and error handling
│   ├── test_router.c     # Unit tests for router pattern matching and sub-routers
│   ├── test_response.c   # Unit tests for headers, cookies, redirects, chunking & trailers
│   ├── test_multipart.c  # Unit tests for multipart/form-data parser
│   ├── test_urlencoded.c # Unit tests for urlencoded parser
│   ├── test_static.c     # Unit tests for static file serving and traversal guards
│   └── test_json.c       # JSON tokenizer, AST building, and serialization tests
├── scripts/               # Benchmarking and utility scripts
│   ├── CLAUDE.md          # Benchmarking tools documentation
│   ├── stress_test.sh     # End-to-end wrk benchmark: build, boot cluster, seed, run (PHASES=ping|churn|read|write)
│   ├── check_docs.sh      # `make check-docs`: keeps lib/API.md in sync with the headers
│   └── wrk_create_todo.lua # wrk load-testing script for POST /api/todos
└── build/                # Out-of-source build outputs (gitignored)
    ├── bin/              # cexpress executable and test runners
    ├── lib/              # libcexpress.a static library
    └── obj/              # Object files (*.o)
```

---

## Roadmap

All 11 architectural milestones have been successfully completed:
- [x] Sub-routers & prefix mounting (`app_mount`, `Router`)
- [x] Additional HTTP verbs (`PUT`, `DELETE`, `PATCH`, `OPTIONS`, `HEAD`)
- [x] Query string parser (`req_get_query`) & URL percent-decoding (`url_decode`)
- [x] Request body dechunking (`Transfer-Encoding: chunked`)
- [x] Static file serving (`app_serve_static`)
- [x] Cookie helpers (`res_set_cookie`, `res_clear_cookie`, `req_get_cookie`)
- [x] Graceful shutdown on `SIGINT`/`SIGTERM` with in-flight request draining
- [x] Streaming & chunked responses (`res_write`, `res_end`, `res_set_trailer`, `res_send_file`)
- [x] Cross-platform event backend (`epoll` for Linux, `kqueue` for macOS/BSD)
- [x] Multi-threaded / multi-process worker model (`SO_REUSEPORT`)
- [x] TLS / HTTPS support (OpenSSL/LibreSSL non-blocking handshake integration)


