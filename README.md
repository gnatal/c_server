# CExpress

A lightweight, high-performance, single-threaded HTTP/1.1 server and web framework written entirely in C (C11), designed with a developer experience inspired by [Express.js](https://expressjs.com/).

> [!NOTE]
> **Library Architecture:**
> The code in `lib/` compiles to a reusable static library (`lib/libcexpress.a`). The code in `app/` is a reference implementation — a SQLite-backed Todo CRUD API — showing how to consume the library. See [`app/CLAUDE.md`](app/CLAUDE.md) for its architecture.

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
- **Maximum Performance & Low Latency**: Native execution with minimal CPU overhead, sub-millisecond response times, and over 202,000 requests/sec with multi-worker concurrency (over 167,000 req/sec single-threaded).
- **Minimal Footprint**: `lib/` itself has zero external dependencies beyond standard C and POSIX APIs (optional OpenSSL for TLS). The bundled demo app additionally links SQLite (embedded, no server process) for its Todo persistence layer.
- **Event-Driven Non-Blocking I/O**: High-performance concurrency powered by native `kqueue` (macOS / BSD) and `epoll` (Linux), following the same architectural pattern as Node.js's underlying `libuv`.
- **Memory Safety & Control**: Explicit bounded buffers, aggressive `const` correctness, bounded I/O guards, and strict dynamic memory allocation tracking.

---

## Performance & Benchmarks

Benchmarked using `wrk` on macOS (Apple Silicon, 8 threads, keep-alive active) running in multi-worker cluster mode (`QUIET=1 WORKERS=4`):

| Concurrency | Threads | Duration | Throughput | Avg Latency | Max Latency | Total Requests | Data Transferred |
|---|---|---|---|---|---|---|---|
| **100 connections** | 8 | 15s | **202,118 req/sec** | **473.86 µs** | 2.18 ms | 3,051,948 | 334.71 MB |
| **1,000 connections** | 8 | 15s | **202,496 req/sec** | **4.92 ms** | 11.57 ms | 3,039,030 | 333.30 MB |
| **5,000 connections** | 8 | 15s | **184,058 req/sec** | **20.00 ms** | 405.31 ms | 2,767,770 | 303.55 MB |

> [!TIP]
> Reproduce these benchmarks with one command (builds, boots a 4-worker cluster, seeds some todos, and runs the full `wrk` sweep against `GET /`, `GET /api/todos`, and `POST /api/todos`):
> ```bash
> scripts/stress_test.sh
> ```
> Tunable via env vars, e.g. `WORKERS=8 CONNS="100 1000 5000 10000" scripts/stress_test.sh` — see [`scripts/CLAUDE.md`](scripts/CLAUDE.md). Or run it manually:
> ```bash
> # Start cluster server with 4 workers in quiet mode
> QUIET=1 WORKERS=4 ./build/bin/cexpress
>
> # In another terminal, run wrk against the Todo API:
> wrk -t8 -c100 -d15s http://127.0.0.1:8080/api/todos
> wrk -t8 -c1000 -d15s http://127.0.0.1:8080/api/todos
> wrk -t8 -c5000 -d15s http://127.0.0.1:8080/api/todos
> ```

---

## Architecture Overview

The codebase is split into two distinct tiers:

```
┌────────────────────────────────────────────────────────┐
│        app/ (Demo App: SQLite-backed Todo CRUD)        │
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
│   - json/        : Recursive-descent JSON AST & writer │
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
- **Mounted Route Serving**: `app_serve_static(app, "/static", "app/public")`.
- **Path-Traversal Protection**: Pure textual `..` segment rejection paired with realpath canonical containment checks against symlink escapes.
- **Directory Index Fallback**: Automatically serves `index.html` for directory requests.
- **Built-in MIME Registry**: Automatic Content-Type resolution for CSS, JS, HTML, PNG, JPEG, SVG, JSON, and binaries.

### 7. Form & Body Parsers
- **Multipart Form Data**: RFC 7578 parser (`lib/multipart.h`) extracting fields and binary file parts with embedded NUL safety.
- **URL-Encoded Forms**: `application/x-www-form-urlencoded` parser (`lib/urlencoded.h`).

### 8. Cookie Management
- **Cookie Parsing**: Access request cookies via `req_get_cookie(req, "session")`.
- **Cookie Setting & Clearing**: `res_set_cookie(res, name, val, &opts)` and `res_clear_cookie(res, name, path)` supporting `Path`, `Domain`, `Max-Age`, `HttpOnly`, `Secure`, and `SameSite` (`Strict`/`Lax`/`None`).

### 9. Built-in JSON Parser & Serializer
- Pure C recursive-descent JSON parser in `lib/json/`.
- Dynamic AST supporting `null`, booleans, numbers, escaped strings, arrays, and objects.
- Serialization back to JSON strings via `json_stringify`.
- **Builder API** for constructing a tree by hand (not just parsing one): `json_new_string`/`json_new_number`/`json_new_bool`/`json_new_object`/`json_new_array` plus `json_object_set`/`json_array_append`, used by the demo app to serialize database rows to JSON.

---

## Prerequisites

- **Operating System**: macOS / BSD (native `kqueue`), Linux (native `epoll`), or Docker on any host.
- **Compiler**: C11 compliant compiler (`gcc-16` or `clang` on macOS; `gcc` on Linux).
- **Build Tool**: GNU `make`.
- **SQLite development headers**: required to build the demo app (`app/db.c`) — `sqlite-dev` on Alpine/Debian, or `brew install sqlite` on macOS. The `Makefile` auto-detects a Homebrew keg, falls back to `pkg-config`, then a bare `-lsqlite3`.
- **Optional**: Docker (for containerized deployment and automated multi-platform verification).

> [!TIP]
> On macOS, `gcc-16` is installed via Homebrew (`brew install gcc`). The default `Makefile` automatically detects macOS (`Darwin`) or Linux (`Linux`), selecting `gcc-16` on Darwin and `gcc` on Linux.

---

## Building & Running

### 1. Compile the Library and Demo
```bash
make
```
This produces:
- `build/lib/libcexpress.a`: The core engine static library.
- `build/bin/cexpress`: The executable demo application (symlinked to `./cexpress`).

### 2. Start the Server
```bash
./cexpress
# or
make run
```
By default, the server listens on port `8080`.

### 3. Running via Docker
CExpress includes an Alpine-based multi-stage `Dockerfile`:
```bash
docker build -t cexpress .
docker run --rm -p 8080:8080 cexpress
```

---

## Running Tests

The project includes **11 isolated test suites** testing all components:

```bash
make test
```

This compiles and executes:
1. `build/bin/test_json`: JSON tokenizer, AST building, and serialization.
2. `build/bin/test_middleware`: Pipeline ordering, short-circuiting, 404 fallthrough, and error handlers.
3. `build/bin/test_router`: Literal paths, parameter extraction, and route table bounds.
4. `build/bin/test_http_parser`: Header parsing, bounds validation, dechunking, and keep-alive.
5. `build/bin/test_connection`: Non-blocking socket I/O, partial reads, socket file streaming, and lifecycle using POSIX `socketpair(2)`.
6. `build/bin/test_response`: Headers, cookies, redirects, chunked streaming, trailers, and HEAD suppression.
7. `build/bin/test_multipart`: RFC 7578 multipart body splitting, file parts, and boundary handling.
8. `build/bin/test_urlencoded`: URL-encoded key-value parsing and percent-decoding.
9. `build/bin/test_static`: Path traversal prevention, symlink escape checks, MIME types, and directory index fallback.
10. `build/bin/test_event_loop`: Event-loop lifecycle, read/write polling across `socketpair`, and idle/shutdown timer verification.
11. `build/bin/test_cluster`: Multi-worker count resolution, `SO_REUSEPORT` multi-bind, concurrent serving, and graceful shutdown.

---

## Configuration

The server supports both runtime environment variables and programmatic configuration:

| Environment Variable | Default Value | Description |
|---|---|---|
| `PORT` | `8080` | TCP port the server binds to (valid range: `1`–`65535`). |
| `WORKERS` | `1` | Number of worker processes (`1` = single process, `auto` or `0` = CPU core auto-detection, `N` = fixed count). |
| `API_KEY` | `my-secret-api-key` | Bearer token verified by the demo authentication middleware. |
| `TODO_DB_PATH` | `todos.db` | Path to the SQLite database file backing the Todo CRUD demo. |

### Running with Custom Configuration
```bash
PORT=3000 API_KEY=super-secret-token ./cexpress
```

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

The demo app (`app/`) is a SQLite-backed Todo CRUD API — see
[`app/CLAUDE.md`](app/CLAUDE.md) for its architecture. Open
`http://localhost:8080/` in a browser for the built-in Todo UI, or drive the
REST API directly:

### 1. Todo UI (GET)
```bash
curl -i http://localhost:8080/
```
A single self-contained page (`app/public/index.html`) served via bounded
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
│   ├── CLAUDE.md         # Engine architecture, data flow, and memory lifecycle
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
│   └── json/             # JSON parsing and AST generation subsystem
│       ├── CLAUDE.md     # JSON subsystem architecture and memory rules
│       ├── json.h        # Public JSON API
│       ├── json_types.h  # AST enum and node structs
│       └── json_*.c      # Parser, value accessors, and stringifier
├── app/                  # Reference demo application: SQLite-backed Todo CRUD
│   ├── CLAUDE.md         # Application wiring, persistence layer & fork-safety notes
│   ├── main.c            # Application entrypoint and route definitions
│   ├── handlers.h/c      # Todo CRUD route handlers
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
│   ├── test_middleware.c # Middleware chain dispatching and error handling
│   ├── test_router.c     # Unit tests for router pattern matching and sub-routers
│   ├── test_response.c   # Unit tests for headers, cookies, redirects, chunking & trailers
│   ├── test_multipart.c  # Unit tests for multipart/form-data parser
│   ├── test_urlencoded.c # Unit tests for urlencoded parser
│   ├── test_static.c     # Unit tests for static file serving and traversal guards
│   └── test_json.c       # JSON tokenizer, AST building, and serialization tests
├── scripts/               # Benchmarking and utility scripts
│   ├── CLAUDE.md          # Benchmarking tools documentation
│   ├── stress_test.sh     # End-to-end wrk benchmark: build, boot cluster, seed, run
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


