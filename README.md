# CExpress

A lightweight, high-performance, single-threaded HTTP/1.1 server and web framework written entirely in C (C11), designed with a developer experience inspired by [Express.js](https://expressjs.com/).

> [!NOTE]
> **Library Architecture:**
> The code in `lib/` compiles to a reusable static library (`lib/libcexpress.a`). The code in `app/` is a reference implementation and demo application showcasing how to consume the library.

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
- **Maximum Performance & Low Latency**: Native execution with minimal CPU overhead, sub-millisecond response times, and over 160,000 requests/sec on a single thread.
- **Minimal Footprint**: Lightweight static binary with zero external dependencies beyond standard C and POSIX APIs.
- **Event-Driven Non-Blocking I/O**: Single-threaded concurrency powered by native `kqueue` (macOS / BSD) and `epoll` (Linux), following the same architectural pattern as Node.js's underlying `libuv`.
- **Memory Safety & Control**: Explicit bounded buffers, aggressive `const` correctness, bounded I/O guards, and strict dynamic memory allocation tracking.

---

## Performance & Benchmarks

Benchmarked using `wrk` on macOS (Apple Silicon, 8 threads, keep-alive active):

| Concurrency | Threads | Duration | Throughput | Avg Latency | Max Latency | Total Requests | Data Transferred |
|---|---|---|---|---|---|---|---|
| **100 connections** | 8 | 15s | **167,902 req/sec** | **573.86 µs** | 4.32 ms | 2,535,418 | 278.07 MB |
| **1,000 connections** | 8 | 15s | **159,373 req/sec** | **6.25 ms** | 22.67 ms | 2,391,896 | 262.33 MB |
| **5,000 connections** | 8 | 15s | **154,621 req/sec** | **26.40 ms** | 109.22 ms | 2,325,396 | 255.03 MB |

> [!TIP]
> Reproduce these benchmarks against the running server using the scripts in `scripts/CLAUDE.md`:
> ```bash
> wrk -t8 -c100 -d15s http://127.0.0.1:8080/
> wrk -t8 -c1000 -d15s http://127.0.0.1:8080/
> wrk -t8 -c5000 -d15s http://127.0.0.1:8080/
> ```

---

## Architecture Overview

The codebase is split into two distinct tiers:

```
┌────────────────────────────────────────────────────────┐
│                   app/ (Demo App)                      │
│   - main.c (Configures routes, routers & boots server) │
│   - handlers.c (Route handlers & streaming endpoints)  │
│   - middlewares.c (Auth, logging, body size guards)    │
└───────────────────────────┬────────────────────────────┘
                            │ links against
┌───────────────────────────▼────────────────────────────┐
│              lib/ (Core Engine - libcexpress.a)        │
│   - connection.c : Portable socket I/O & lifecycle     │
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

### 3. Non-Blocking Event-Driven Networking
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

---

## Prerequisites

- **Operating System**: macOS / BSD (native `kqueue`), Linux (native `epoll`), or Docker on any host.
- **Compiler**: C11 compliant compiler (`gcc-16` or `clang` on macOS; `gcc` on Linux).
- **Build Tool**: GNU `make`.
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

The project includes **10 isolated test suites** testing all components:

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

---

## Configuration

The server supports both runtime environment variables and programmatic configuration:

| Environment Variable | Default Value | Description |
|---|---|---|
| `PORT` | `8080` | TCP port the server binds to (valid range: `1`–`65535`). |
| `API_KEY` | `my-secret-api-key` | Bearer token verified by the demo authentication middleware. |

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

### 1. Basic Welcome Route (GET)
```bash
curl -i http://localhost:8080/
```

### 2. Path Parameter Route (GET)
```bash
curl -i http://localhost:8080/users/42
```

### 3. Chunked Response Streaming with Trailers (GET)
```bash
curl -i --raw http://localhost:8080/stream
```
**Response:**
```http
HTTP/1.1 200 OK
Content-Type: text/plain
Transfer-Encoding: chunked
Connection: keep-alive
Trailer: Server-Timing

f
chunk 1: hello

19
chunk 2: streaming world

e
chunk 3: done

0
Server-Timing: demo;dur=12.5

```

### 4. Bounded File Streaming (GET)
```bash
curl -i http://localhost:8080/download
```

### 5. Protected JSON Echo Route (POST)
```bash
curl -i -X POST \
  -H "Authorization: Bearer my-secret-api-key" \
  -H "Content-Type: application/json" \
  -d '{"name":"Ada Lovelace","skills":["Math","Computing"]}' \
  http://localhost:8080/echo/json
```

### 6. Sub-Router Endpoint (GET)
```bash
curl -i -H "Authorization: Bearer my-secret-api-key" http://localhost:8080/api/status
```

### 7. Cookie Session Demo
```bash
# Set cookie
curl -i -c cookies.txt http://localhost:8080/login

# Read cookie
curl -i -b cookies.txt http://localhost:8080/whoami

# Clear cookie
curl -i -b cookies.txt http://localhost:8080/logout
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
├── CLAUDE.md             # Project standards, coding guidelines, and workflow rules
├── AGENTS.md             # Agent context and workflow guidelines
├── pending.txt           # Feature tracking and architectural backlog
├── plan.md               # Architectural roadmap and task estimates
├── lib/                  # Reusable CExpress engine (builds to build/lib/libcexpress.a)
│   ├── CLAUDE.md         # Engine architecture, data flow, and memory lifecycle
│   ├── app_types.h       # Struct definitions, event loop types, function pointer signatures
│   ├── event_loop.h      # Cross-platform event-loop abstraction
│   ├── event_loop_kqueue.c # Native BSD/macOS kqueue backend
│   ├── event_loop_epoll.c  # Native Linux epoll backend (timerfd + signalfd)
│   ├── connection.h/c    # Portable non-blocking socket I/O & graceful shutdown
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
├── app/                  # Reference demo application
│   ├── CLAUDE.md         # Application wiring and middleware architecture
│   ├── main.c            # Application entrypoint and route definitions
│   ├── handlers.h/c      # Route handlers, streaming & download handlers
│   ├── middlewares.h/c   # Logger, body size guard, and authentication middlewares
│   └── public/           # Static asset directory (served via app_serve_static)
│       ├── index.html    # Demo HTML document
│       └── style.css     # Demo stylesheet
├── tests/                # Isolated test suites (built into build/bin/test_*)
│   ├── CLAUDE.md         # Test harness architecture and socket mocking strategy
│   ├── test_connection.c # Socket I/O and lifecycle tests via socketpair(2)
│   ├── test_event_loop.c # Event-loop abstraction tests (lifecycle, I/O, timers)
│   ├── test_http_parser.c# Unit tests for HTTP parser and request dechunking
│   ├── test_middleware.c # Middleware chain dispatching and error handling
│   ├── test_router.c     # Unit tests for router pattern matching and sub-routers
│   ├── test_response.c   # Unit tests for headers, cookies, redirects, chunking & trailers
│   ├── test_multipart.c  # Unit tests for multipart/form-data parser
│   ├── test_urlencoded.c # Unit tests for urlencoded parser
│   ├── test_static.c     # Unit tests for static file serving and traversal guards
│   └── test_json.c       # JSON tokenizer, AST building, and serialization tests
├── scripts/              # Benchmarking and utility scripts
│   ├── CLAUDE.md         # Benchmarking tools documentation
│   └── wrk_echo_json.lua # wrk load-testing script
└── build/                # Out-of-source build outputs (gitignored)
    ├── bin/              # cexpress executable and test runners
    ├── lib/              # libcexpress.a static library
    └── obj/              # Object files (*.o)
```

---

## Roadmap

Check [pending.txt](pending.txt) and [plan.md](plan.md) for the complete architectural backlog:
- [x] Sub-routers & prefix mounting (`app_mount`, `Router`)
- [x] Additional HTTP verbs (`PUT`, `DELETE`, `PATCH`, `OPTIONS`, `HEAD`)
- [x] Query string parser (`req_get_query`) & URL percent-decoding (`url_decode`)
- [x] Request body dechunking (`Transfer-Encoding: chunked`)
- [x] Static file serving (`app_serve_static`)
- [x] Cookie helpers (`res_set_cookie`, `res_clear_cookie`, `req_get_cookie`)
- [x] Graceful shutdown on `SIGINT`/`SIGTERM` with in-flight request draining
- [x] Streaming & chunked responses (`res_write`, `res_end`, `res_set_trailer`, `res_send_file`)
- [x] Cross-platform event backend (`epoll` for Linux, `kqueue` for macOS/BSD)
- [ ] Multi-threaded / multi-process worker model (`SO_REUSEPORT`)
- [ ] TLS / HTTPS support (OpenSSL/LibreSSL non-blocking handshake integration)

