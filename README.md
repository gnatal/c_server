# CExpress

A lightweight, high-performance, single-threaded HTTP/1.1 server and web framework written entirely in C (C11), designed with a developer experience inspired by [Express.js](https://expressjs.com/).

> [!NOTE]
> **Future Library Architecture:**
> The code in `lib/` compiles to a reusable static library (`lib/libcexpress.a`). In future releases, this engine will be decoupled and distributed as an independent, embeddable C library. The code in `app/` is strictly a reference implementation and demo application showcasing how to consume the library.

---

## Table of Contents
- [Purpose](#purpose)
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

Modern backend applications often rely on high-level runtimes like Node.js or Go. This project aims to bring the ergonomic, developer-friendly routing and middleware design of **Express.js** directly to **C**, providing:
- **Maximum Performance & Low Latency**: Native execution with minimal CPU overhead and sub-millisecond response times.
- **Minimal Footprint**: Lightweight static binary with zero runtime external dependencies beyond standard C and POSIX APIs.
- **Event-Driven Non-Blocking I/O**: Single-threaded concurrency powered by `kqueue` (macOS / BSD), following the same architecture as Node.js's underlying `libuv`.
- **Memory Safety & Control**: Explicit bounded buffers, aggressive `const` correctness, bounded I/O guards, and strict dynamic memory allocation tracking.

---

## Architecture Overview

The codebase is split into two distinct tiers:

```
┌────────────────────────────────────────────────────────┐
│                   app/ (Demo App)                      │
│   - main.c (Configures & boots server)                 │
│   - handlers.c (Route handlers)                        │
│   - middlewares.c (Auth, logging, body size guards)    │
└───────────────────────────┬────────────────────────────┘
                            │ links against
┌───────────────────────────▼────────────────────────────┐
│              lib/ (Core Engine - libcexpress.a)        │
│   - connection.c : kqueue event loop & socket I/O      │
│   - httpParser.c : HTTP/1.1 parsing & buffer guards    │
│   - router.c     : Trie/segment pattern matcher        │
│   - middleware.c : Dispatch & middleware pipeline      │
│   - response.c   : Response builder (send, json)       │
│   - json/        : Lightweight recursive-descent JSON  │
└────────────────────────────────────────────────────────┘
```

---

## Features

### 1. Express-Style Routing
- **Method-based registration**: Convenient `app_get()`, `app_post()`, and generic `app_add_route()`.
- **Path parameter extraction**: Named segment variables (e.g. `/users/:id`, `/orgs/:orgId/repos/:repoName`).
- **Parameter lookup**: Clean access via `req_get_param(req, "id")`.
- **Deny-by-default**: Unmatched paths and methods securely fall through to `404 Not Found`.

### 2. Middleware & Centralized Error Pipeline
- **Chained execution**: Middleware functions execute sequentially with `chain_next(chain)`.
- **Short-circuiting**: Middleware can terminate requests early by responding directly (e.g., returning 401 or 403).
- **Post-dispatch access**: Synchronous pipeline flow enables middlewares to inspect finalized response statuses (such as `mw_logger` logging `METHOD PATH -> STATUS`).
- **Centralized error handling**: `chain_error(chain, status, message)` mirrors Express's `(err, req, res, next)` error-handling model.
- **Built-in demo middlewares**:
  - `mw_logger`: Access logging.
  - `mw_body_size_guard`: Independent request body size bounding.
  - `mw_authenticate`: Constant-time Bearer token verification to protect against timing attacks.
  - `error_handler_json`: Centralized JSON error serialization.

### 3. Non-Blocking Event-Driven Networking
- Single-threaded event loop utilizing **BSD/macOS `kqueue`**.
- Non-blocking sockets (`O_NONBLOCK` + `TCP_NODELAY`).
- Connection state machine with level-triggered socket acceptance and write queue flushing.
- **Keep-Alive Support**: HTTP/1.1 persistent connections keep sockets open for multiple requests; explicit `Connection: close` tears down cleanly.

### 4. Robust HTTP/1.1 Parser & Security Guards
- Pure, side-effect-free parser (`parse_http_request`) operating on read-only byte buffers.
- Query string extraction (`req->path` and `req->query`).
- Strict buffer bounds enforcement (`BUF_SIZE = 8192`):
  - Rejects oversized/negative `Content-Length` with `400 Bad Request`.
  - Rejects header overflow / Slowloris patterns with `431 Request Header Fields Too Large`.
- Banned unsafe C string functions (no `strcpy`, `strcat`, `sprintf`, or `gets`).

### 5. Built-in JSON Parser & Serializer
- Pure C recursive-descent JSON parser in `lib/json/`.
- Dynamic AST supporting `null`, booleans, numbers, escaped strings, arrays, and objects.
- Serialization back to JSON strings via `json_stringify`.

---

## Prerequisites

- **Operating System**: macOS or BSD (requires `kqueue`).
- **Compiler**: C11 compliant compiler (`gcc-16` or `clang`).
- **Build Tool**: GNU `make`.

> [!TIP]
> On macOS, `gcc-16` is installed via Homebrew (`brew install gcc`). The default `Makefile` is configured with `CC = gcc-16`.

---

## Building & Running

### 1. Compile the Library and Demo
```bash
make
```
This produces:
- `lib/libcexpress.a`: The core engine static library.
- `cexpress`: The executable demo application.

### 2. Start the Server
```bash
./cexpress
# or
make run
```
By default, the server listens on port `8080`.

---

## Running Tests

The project includes 5 isolated test suites testing all components:

```bash
make test
```

This compiles and executes:
1. `lib/json/json_test`: JSON tokenizer, AST building, and serialization.
2. `lib/middleware_test`: Pipeline ordering, short-circuiting, 404 fallthrough, and error handlers.
3. `lib/router_test`: Literal paths, parameter extraction, and route table bounds.
4. `lib/http_parser_test`: Header parsing, bounds validation, and connection keep-alive determination.
5. `lib/connection_test`: Non-blocking socket I/O, partial reads, buffer overflow guards, and keep-alive lifecycle using POSIX `socketpair(2)` and isolated `kqueue`.

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

### Programmatic Configuration
- **Port**: Set `app.config.port = 3000;` before calling `app_listen(&app, app.config.port);`.
- **API Key**: Call `mw_authenticate_set_key("my-token");` at startup.
- **Engine Limits**: Compile-time constants in `lib/appTypes.h`:
  - `BUF_SIZE`: Fixed buffer per connection (default: `8192` bytes).
  - `MAX_ROUTES`: Maximum registered routes (default: `32`).
  - `MAX_MIDDLEWARES`: Maximum app-wide middlewares (default: `16`).
  - `MAX_PARAMS`: Maximum path parameters captured per route (default: `8`).
  - `MAX_CONNECTIONS`: Maximum concurrent connection descriptors (default: `16384`).

---

## Quick Start & Example Usage

Start the server:
```bash
PORT=8080 ./cexpress
```

In another terminal, test the demo routes:

### 1. Welcome / Home Route (GET)
```bash
curl -i -H "Authorization: Bearer my-secret-api-key" http://localhost:8080/
```
**Response:**
```http
HTTP/1.1 200 OK
Content-Type: text/plain
Content-Length: 26
Connection: keep-alive

Welcome to the home page!
```

### 2. Path Parameter Route (GET)
```bash
curl -i -H "Authorization: Bearer my-secret-api-key" http://localhost:8080/users/42
```
**Response:**
```http
HTTP/1.1 200 OK
Content-Type: text/plain
Content-Length: 17
Connection: keep-alive

User ID requested: 42
```

### 3. JSON Echo Route (POST)
```bash
curl -i -X POST \
  -H "Authorization: Bearer my-secret-api-key" \
  -H "Content-Type: application/json" \
  -d '{"name":"Ada Lovelace","skills":["Math","Computing"]}' \
  http://localhost:8080/echo/json
```
**Response:**
```http
HTTP/1.1 200 OK
Content-Type: application/json
Content-Length: 53
Connection: keep-alive

{"name":"Ada Lovelace","skills":["Math","Computing"]}
```

### 4. Authentication Middleware in Action (401)
```bash
# Missing token:
curl -i http://localhost:8080/

# Invalid token:
curl -i -H "Authorization: Bearer wrong-key" http://localhost:8080/
```
**Response:**
```http
HTTP/1.1 401 Unauthorized
Content-Type: application/json
Content-Length: 41
Connection: keep-alive

{"error":"Unauthorized: Invalid API key"}
```

---

## Project Layout

```
.
├── Makefile              # Build rules for lib, demo app, and all test suites
├── README.md             # Project documentation
├── CLAUDE.md             # Project standards, coding guidelines, and workflow rules
├── pending.txt           # Feature tracking and architectural backlog
├── lib/                  # Reusable CExpress engine (builds to libcexpress.a)
│   ├── appTypes.h        # Struct definitions, function pointer signatures, and limits
│   ├── connection.h/c    # kqueue event loop, non-blocking socket I/O & lifecycle
│   ├── httpParser.h/c    # Pure HTTP/1.1 parser, query string and header parsing
│   ├── router.h/c        # Path pattern matching and route table dispatch
│   ├── response.h/c      # Response construction (res_send, res_json, res_status)
│   ├── middleware.h/c    # MiddlewareChain and dispatch pipeline
│   ├── json/             # JSON parsing and AST generation subsystem
│   ├── router_test.c     # Unit tests for router pattern matching
│   ├── http_parser_test.c# Unit tests for HTTP parser
│   └── connection_test.c # Socket I/O and lifecycle tests via socketpair(2)
└── app/                  # Reference demo application
    ├── main.c            # Application entrypoint and route definitions
    ├── handlers.h/c      # Route handlers
    └── middlewares.h/c   # Logger, body size guard, and authentication middlewares
```

---

## Roadmap

Check [pending.txt](pending.txt) for the full architectural roadmap:
- [ ] Sub-routers / prefix-scoped mounting (`app.use("/api", subrouter)`)
- [ ] Additional HTTP verbs (`PUT`, `DELETE`, `PATCH`, `OPTIONS`, `HEAD`)
- [ ] 405 Method Not Allowed support
- [ ] Query string map parser (`req_get_query(name)`)
- [ ] URL percent-decoding
- [ ] Cross-platform event backend (`epoll` for Linux)
