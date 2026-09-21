# CExpress API & Developer Documentation

Welcome to the **CExpress** documentation. CExpress brings the developer ergonomics and modular architecture of [Express.js](https://expressjs.com/) to native C (C11), powered by non-blocking event-driven I/O: `kqueue` on macOS/BSD and `io_uring` readiness polling on Linux.

For a one-line-per-function index see [`lib/API.md`](lib/API.md); for engine internals, limits and known gaps see [`lib/CLAUDE.md`](lib/CLAUDE.md); for tested copy-paste recipes see [`lib/examples/cookbook.c`](lib/examples/cookbook.c).

---

## Table of Contents

1. [Getting Started](#1-getting-started)
   - [Including the Umbrella Header](#including-the-umbrella-header)
   - [Compiling & Linking](#compiling--linking)
   - [Hello World Example](#hello-world-example)
2. [Routing](#2-routing)
   - [Basic Methods](#basic-methods)
   - [Route Parameters (`:param`)](#route-parameters-param)
   - [Wildcard Routes (`*`)](#wildcard-routes-)
   - [Match Order](#match-order)
   - [Sub-Routers (`app_mount`)](#sub-routers-app_mount)
3. [Middleware Pipeline](#3-middleware-pipeline)
   - [How Middleware Works](#how-middleware-works)
   - [App-Wide Middleware](#app-wide-middleware)
   - [Prefix-Scoped Middleware](#prefix-scoped-middleware)
   - [Per-Route Middleware](#per-route-middleware)
   - [Centralized Error Handling](#centralized-error-handling)
4. [Request & Response API](#4-request--response-api)
   - [The `Request` Object](#the-request-object)
   - [The `Response` Object](#the-response-object)
   - [Sending Responses (`res_send`, `res_json`)](#sending-responses)
   - [Custom Headers & Status](#custom-headers--status)
   - [Cookies & Sessions](#cookies--sessions)
   - [Redirects](#redirects)
5. [Memory Model](#5-memory-model)
6. [Advanced Features](#6-advanced-features)
   - [Streaming & Chunked Responses](#streaming--chunked-responses)
   - [Chunked Trailers](#chunked-trailers)
   - [Bounded File Streaming (`res_send_file`)](#bounded-file-streaming-res_send_file)
   - [Static File Serving (`app_serve_static`)](#static-file-serving-app_serve_static)
   - [Multipart & Form Data Parsing](#multipart--form-data-parsing)
   - [JSON with yyjson](#json-with-yyjson)
   - [Multi-Worker Concurrency](#multi-worker-concurrency-so_reuseport)
   - [Graceful Shutdown](#graceful-shutdown)
7. [Limits and Error Responses](#7-limits-and-error-responses)
8. [API Reference Quick Index](#8-api-reference-quick-index)

---

## 1. Getting Started

### Including the Umbrella Header
CExpress provides a single umbrella header:

```c
#include "cexpress.h"
```

This includes all core subsystems: routing, response helpers, middleware chains, HTTP parser, static files, multipart, URL-encoded forms, the per-connection arena, TLS, and the vendored yyjson JSON library.

### Compiling & Linking
Build the library once with `make` in the CExpress checkout (it produces `build/lib/libcexpress.a`), then compile your application files and link against it. Also link OpenSSL when the library was built with TLS (the default when OpenSSL is found) and, on Linux, liburing:

```bash
# macOS (Homebrew gcc; adjust the OpenSSL prefix to your machine)
gcc-16 -Wall -Wextra -std=c11 -O2 -Ipath/to/cexpress/lib -o my_app main.c \
    path/to/cexpress/build/lib/libcexpress.a -L/opt/homebrew/opt/openssl@3/lib -lssl -lcrypto

# Linux
gcc -Wall -Wextra -std=c11 -O2 -D_GNU_SOURCE -Ipath/to/cexpress/lib -o my_app main.c \
    path/to/cexpress/build/lib/libcexpress.a -lssl -lcrypto -luring
```

[`importing.md`](importing.md) has a complete, tested Makefile that does this for you.

### Hello World Example

```c
#include "cexpress.h"
#include <stdio.h>

void handler_hello(const Request *req, Response *res) {
    (void)req;
    res_send(res, "Hello, World!");
}

int main(void) {
    App app;
    app_init(&app);

    app_get(&app, "/", handler_hello);

    printf("Server listening on http://localhost:8080\n");
    app_listen(&app, 8080);

    /* Cleans up remaining connections, routes and event loop resources upon shutdown */
    app_destroy(&app);
    return 0;
}
```

---

## 2. Routing

Handlers in CExpress adhere to the following signature:
```c
typedef void (*Handler)(const Request *req, Response *res);
```
Handlers receive a read-only `const Request *req` and a mutable `Response *res`. Handlers are pure: they write responses into buffers rather than performing raw socket I/O directly.

### Basic Methods
Register route handlers for specific HTTP verbs:

```c
app_get(&app, "/items", handler_get_items);
app_post(&app, "/items", handler_create_item);
app_put(&app, "/items/:id", handler_update_item);
app_patch(&app, "/items/:id", handler_patch_item);
app_delete(&app, "/items/:id", handler_delete_item);
```

- **`HEAD` Requests**: Automatically supported. `HEAD` requests execute the matched `GET` handler, emitting all headers while suppressing the body per RFC 7230 §3.3.3.
- **`OPTIONS` Requests**: Automatically supported. Returns `200 OK` with an empty body and the appropriate `Allow` header matching registered methods.
- **`405 Method Not Allowed`**: If a path matches but the method does not, CExpress returns `405` with the `Allow` header.
- **`404 Not Found`**: Anything else. There is no fallback route: unmatched requests are denied.

### Route Parameters (`:param`)
Extract named segments from URL paths:

```c
app_get(&app, "/users/:userId/posts/:postId", handler_post);

void handler_post(const Request *req, Response *res) {
    const char *user_id = req_get_param(req, "userId");
    const char *post_id = req_get_param(req, "postId");

    char body[128];
    snprintf(body, sizeof(body), "User: %s, Post: %s", user_id, post_id);
    res_send(res, body);
}
```

Use the **same parameter name at the same position in every route**. Routes are stored in a tree per method, and the capture name comes from the first route registered at that position: with `"/orders/:id/items"` registered first, `"/orders/:oid/notes"` captures under `id`, and `req_get_param(req, "oid")` returns `NULL`.

### Wildcard Routes (`*`)
A trailing `*` segment captures the remainder of the path (one or more segments; the bare prefix does not match):

```c
app_get(&app, "/files/*", handler_files);

void handler_files(const Request *req, Response *res) {
    /* req->path contains the full path, e.g. "/files/docs/readme.txt" */
    res_send(res, req->path);
}
```

A `*` in the middle of a pattern (`"/users/*/edit"`) matches exactly one segment and captures nothing.

### Match Order
For each path segment the router tries a literal segment first, then a `:param`, then a trailing `*`, backtracking when a branch dead-ends. So a more specific route wins **regardless of registration order**: `"/users/me"` handles `/users/me` even if `"/users/:id"` was registered first. Registering the same pattern twice keeps the first and prints a warning. Empty segments are ignored (`/users/` = `/users`), and the path is percent-decoded before matching.

### Sub-Routers (`app_mount`)
Modularize your application routes by creating standalone `Router` instances and mounting them under a path prefix (identical to Express's `app.use('/api', apiRouter)`):

```c
Router api_router;
router_init(&api_router);

/* Relative to "/api" */
router_get(&api_router, "/status", handler_api_status);
router_get(&api_router, "/users/:id", handler_api_user);

/* Mounts to GET /api/status and GET /api/users/:id */
app_mount(&app, "/api", &api_router);
```

`app_mount` copies the routes, so the `Router` may be a stack local. Routers do not nest.

---

## 3. Middleware Pipeline

Middleware functions match the signature:
```c
typedef void (*Middleware)(const Request *req, Response *res, MiddlewareChain *chain);
```

### How Middleware Works
- Call `chain_next(chain)` to pass control to the next middleware or handler.
- Stop execution by writing a response directly (e.g. `res_status(res, 401); res_send(res, "Unauthorized");`) and **not** calling `chain_next`.
- Post-processing: Code placed *after* `chain_next(chain)` executes after the handler has run (e.g., access loggers inspecting `res->status`).

### App-Wide Middleware
Runs for every incoming request, including ones that end in a 404:

```c
void mw_logger(const Request *req, Response *res, MiddlewareChain *chain) {
    /* Process downstream */
    chain_next(chain);

    /* Post-execution logging */
    printf("%s %s -> %d\n", req->method, req->path, res->status);
}

app_use(&app, mw_logger);
```

### Prefix-Scoped Middleware
Runs only for requests matching a URL prefix (at a segment boundary: `/admin` matches `/admin/x`, not `/administrator`):

```c
/* Only executes for requests starting with "/admin" */
app_use_prefix(&app, "/admin", mw_admin_auth);
```

### Per-Route Middleware
Attach middleware specifically to a single route:

```c
app_post_mw(&app, "/checkout", handler_checkout, (Middleware[]){mw_require_auth, mw_rate_limit}, 2);
```

Sub-routers also support router-level middleware. It becomes prefix-scoped app-wide middleware when the router is mounted:
```c
router_use(&api_router, mw_require_auth);
```

### Centralized Error Handling
Trigger centralized error processing with `chain_error`:

```c
void mw_guard(const Request *req, Response *res, MiddlewareChain *chain) {
    (void)req; (void)res;
    if (!authorized) { /* e.g. check a header or cookie via req */
        chain_error(chain, 403, "Access Forbidden");
        return;
    }
    chain_next(chain);
}
```

Register a custom error handler (the last `app_use_error` wins). Build JSON with a JSON library, not `snprintf`, so the message is escaped:
```c
void my_error_handler(int status, const char *message, const Request *req, Response *res) {
    (void)req;
    yyjson_alc alc = arena_yyjson_alc(&res->conn->arena);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(&alc);
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, obj);
    yyjson_mut_obj_add_int(doc, obj, "status", status);
    yyjson_mut_obj_add_str(doc, obj, "error", message);

    char *json = yyjson_mut_write(doc, 0, NULL);
    res_status(res, status);
    if (json != NULL) {
        res_json(res, json);
        free(json);
    } else {
        res_json(res, "{\"error\":\"internal error\"}");
    }
    yyjson_mut_doc_free(doc);
}

app_use_error(&app, my_error_handler);
```

Route handlers cannot call `chain_error` (they have no chain); they set the status and body themselves.

---

## 4. Request & Response API

### The `Request` Object
The `Request` struct (`req`) provides read-only request metadata:
- `req->method`: HTTP verb (`"GET"`, `"POST"`, `"PUT"`, etc.).
- `req->path`: Percent-decoded URL path without the query (e.g. `"/users"`).
- `req->query`: Raw query string (e.g. `"sort=asc&limit=10"`).
- `req->version`: `"HTTP/1.1"` or `"HTTP/1.0"`.
- `req->body`: Request body bytes, NUL-terminated (an empty string if there is no body).
- `req->content_length`: Body length in bytes. Use it, not `strlen`, for binary bodies.

Everything reachable from `req` lives until the handler returns; see [Memory Model](#5-memory-model).

#### Helpers
- `req_get_param(req, "paramName")`: Lookup named route variable.
- `req_get_query(req, "queryKey")`: Lookup query parameter by key (decoded, `+` becomes a space).
- `req_get_header(req, "Header-Name")`: Case-insensitive header lookup.
- `req_get_cookie(req, "cookieName")`: Lookup cookie by name.

### The `Response` Object

#### Sending Responses
```c
/* Send text/plain */
res_send(res, "Hello World");

/* Send application/json */
res_json(res, "{\"success\":true}");

/* Send raw binary data with custom Content-Type */
res_send_bytes(res, "image/png", png_bytes, png_len);
```
All three copy the bytes, so the buffer you pass can be a stack array or freed straight away.

#### Custom Headers & Status
```c
res_status(res, 201);
res_set_header(res, "X-Server-Name", "CExpress-Edge");
```
*(Reserved headers `Content-Length` and `Connection` are managed automatically by the response layer. A header name or value containing a control character, including CR and LF, is dropped, which prevents response splitting from request data.)*

#### Cookies & Sessions
```c
CookieOptions opts = {
    .max_age = 86400,          /* 24 hours */
    .path = "/",
    .http_only = 1,
    .secure = 1,
    .same_site = COOKIE_SAMESITE_STRICT
};
res_set_cookie(res, "session_id", "xyz123", &opts);

/* Clear cookie immediately */
res_clear_cookie(res, "session_id", "/");
```

#### Redirects
```c
/* 302 Found (default) */
res_redirect(res, 0, "/dashboard");

/* Explicit 301 Moved Permanently */
res_redirect(res, 301, "https://example.com");
```
`res_redirect` refuses a target containing CR/LF (500) but does not check for open redirects: validate targets that come from users (see the `/go` recipe in the cookbook).

---

## 5. Memory Model

Each connection owns a 64 KiB **arena** (a bump allocator) that lives inside the same allocation as the connection. The engine allocates everything that lives for one request from it: the request body, the response bytes, and any yyjson document you build with `arena_yyjson_alc(&res->conn->arena)`. When a keep-alive response has been fully written the arena is reset in O(1); when the connection closes it is destroyed. A request that needs more than the arena has left transparently falls back to `malloc`, freed at the same moment.

What that means for handler code:
- Never `free` `req->body`, `req_get_*` results, or anything from `arena_alloc` / an arena-backed yyjson document.
- Never keep such a pointer in a global or a struct that outlives the handler. Copy what you need.
- `res_send`, `res_json`, `res_send_bytes` copy their argument, so your own buffers can be freed or reused immediately.
- The one thing you **do** free: the string returned by `yyjson_mut_write`, which comes from libc `malloc`.

Details and the full ownership table: [`lib/CLAUDE.md`](lib/CLAUDE.md), rationale and costs (about 25 KB resident per connection on macOS): [`tradeoffs.md`](tradeoffs.md).

---

## 6. Advanced Features

### Streaming & Chunked Responses
Stream incremental chunks using standard HTTP/1.1 `Transfer-Encoding: chunked`:

```c
void handler_stream(const Request *req, Response *res) {
    (void)req;
    res_set_header(res, "Content-Type", "text/plain");

    /* Send chunks incrementally */
    res_write(res, "chunk 1...\n", 11);
    res_write(res, "chunk 2...\n", 11);
    res_write(res, "final chunk\n", 12);

    /* Emits the terminal 0\r\n chunk */
    res_end(res);
}
```
Chunks accumulate in the connection's output buffer (up to about 10 MiB per response) and are written after the handler returns; the handler never blocks on the socket.

### Chunked Trailers
Attach trailers (e.g. `Server-Timing`, checksums) to chunked responses:

```c
res_set_trailer(res, "Server-Timing", "db;dur=14.2, render;dur=3.1");
res_write(res, "data", 4);
res_end(res);
```

### Bounded File Streaming (`res_send_file`)
Stream files directly from disk through the event loop in 16 KiB bounded chunks without buffering entire files into RAM:

```c
void handler_download(const Request *req, Response *res) {
    (void)req;
    if (res_send_file(res, "application/pdf", "reports/annual.pdf") != 0) {
        res_status(res, 404);
        res_send(res, "File not found");
    }
}
```
`res_send_file` does no path checking: never pass it a path built from request data. The path is resolved against the working directory.

### Static File Serving (`app_serve_static`)
Mount an entire directory of public assets with automatic MIME-type detection, path traversal defense, and `index.html` fallback:

```c
/* GET /static/style.css -> serves app/public/style.css */
app_serve_static(&app, "/static", "app/public");
```
The root is resolved when the route is registered; a missing directory registers nothing (and logs it), so every request under the prefix then answers 404. Files up to 50 MiB are read into memory per request. Use `res_send_file` for large files.

### Multipart & Form Data Parsing

#### URL-Encoded Forms (`application/x-www-form-urlencoded`)
```c
UrlEncodedForm form;
parse_urlencoded_body(req->body, (size_t)req->content_length, &form);

for (int i = 0; i < form.field_count; i++) {
    printf("%s = %s\n", form.field_names[i], form.field_values[i]);
}
```

#### Multipart Uploads (`multipart/form-data`)
```c
char boundary[MAX_BOUNDARY_LEN];
if (!multipart_parse_boundary(req_get_header(req, "Content-Type"), boundary, sizeof(boundary))) {
    res_status(res, 415);
    res_send(res, "expected multipart/form-data");
    return;
}

MultipartForm form;
parse_multipart_body(req->body, (size_t)req->content_length, boundary, &form);

for (int i = 0; i < form.part_count; i++) {
    const MultipartPart *part = &form.parts[i];
    if (part->filename[0] != '\0') {
        /* part->data points into req->body, is not NUL-terminated: use data_len */
        printf("Uploaded file: %s (%zu bytes)\n", part->filename, part->data_len);
    }
}
```

### JSON with yyjson
CExpress vendors [yyjson](https://github.com/ibireme/yyjson) (`lib/vendor/yyjson`, version 0.13.0) and includes its header from `cexpress.h`; there is no separate JSON layer. Give yyjson the connection arena as its allocator so documents need no freeing.

**Reading a request body:**
```c
yyjson_alc alc = arena_yyjson_alc(&res->conn->arena);
yyjson_doc *doc = yyjson_read_opts(req->body, (size_t)req->content_length, 0, &alc, NULL);
if (doc == NULL) {
    res_status(res, 400);
    res_json(res, "{\"error\":\"invalid json\"}");
    return;
}
yyjson_val *root = yyjson_doc_get_root(doc);
const char *title = yyjson_get_str(yyjson_obj_get(root, "title")); /* NULL if missing or not a string */
if (title != NULL) {
    printf("Title: %s\n", title); /* points into the document: use it before the handler returns */
}
yyjson_doc_free(doc); /* a no-op with the arena allocator, still correct */
```

**Building a response:**
```c
yyjson_alc alc = arena_yyjson_alc(&res->conn->arena);
yyjson_mut_doc *doc = yyjson_mut_doc_new(&alc);
yyjson_mut_val *todo = yyjson_mut_obj(doc);
yyjson_mut_doc_set_root(doc, todo);
yyjson_mut_obj_add_int(doc, todo, "id", 42);
yyjson_mut_obj_add_str(doc, todo, "title", "Buy milk");   /* the string is borrowed, not copied */
yyjson_mut_obj_add_bool(doc, todo, "done", 0);

char *json_str = yyjson_mut_write(doc, 0, NULL); /* libc-malloc'd, even with the arena allocator */
if (json_str != NULL) {
    res_json(res, json_str); /* copies the bytes */
    free(json_str);          /* required: forgetting this leaks one string per request */
} else {
    res_status(res, 500);
    res_send(res, "encoding failed");
}
yyjson_mut_doc_free(doc);
```
Strings you pass to `yyjson_mut_obj_add_str` must stay valid until `yyjson_mut_write` returns (use the `..._strcpy` variants to copy). Recipes for arrays, error bodies and content-type checks are in the cookbook.

### Multi-Worker Concurrency (`SO_REUSEPORT`)
CExpress scales linearly across CPU cores using a multi-process worker model powered by kernel-level `SO_REUSEPORT` socket load balancing.
1. Master process manages worker lifecycle, tracks PIDs, and intercepts termination signals.
2. Each worker opens an independent listening socket with `SO_REUSEPORT` on the same port and runs an isolated event loop.
3. Master automatically detects crashed workers and respawns replacement workers.
4. Graceful cluster shutdown coordinates draining across all workers within a 5-second deadline.
5. See [concurrency.md](concurrency.md) for full architecture details and container guidelines.

Enable it with `app.config.workers = N` (`0` means one per CPU core) before `app_listen`.

**Worker lifecycle hooks (`app_on_worker_start`)**: a resource opened once in
`main()` before `app_listen()` gets duplicated into every forked worker along
with the rest of that process's memory - fine for most state, but unsafe for
a resource with its own live OS-level state (a database connection is the
motivating case; see `examples/todo_sqlite/db.c` for a full worked example with SQLite).
Register a callback instead of opening such a resource directly:
```c
void my_resource_init(void) {
    /* Runs once per worker process, always after any fork has already
     * happened - safe to open a private, per-process connection/handle here. */
}

app_on_worker_start(&app, my_resource_init);
app_listen(&app, port);
```
`app_listen_worker` (the function every serving process - standalone or a
forked cluster worker - always calls before starting its event loop) runs
every registered hook, in registration order, before doing anything else.

### Graceful Shutdown
CExpress intercepts `SIGINT` (`Ctrl+C`) and `SIGTERM` directly in the native event loop (`EVFILT_SIGNAL` on kqueue, `signalfd` on Linux):
1. Stops accepting incoming TCP connections immediately.
2. Closes idle keep-alive connections.
3. Drains in-flight requests and file streams.
4. Triggers a 5-second deadline timer before force-exiting if clients stall.
5. Returns cleanly from `app_listen` to allow `app_destroy` to release all memory.

A second signal during the drain exits immediately.

---

## 7. Limits and Error Responses

All limits are compile-time constants in `lib/app_types.h`. Input past a limit is rejected or truncated, never overflowed.

| Situation | Result |
|---|---|
| Request line and headers larger than 8 KiB | `431`, connection closed |
| Path longer than 255 bytes | `414`, connection closed |
| Body larger than 10 MiB (`Content-Length` or decoded chunked) | `413`, connection closed |
| More than 32 request headers | `400`, connection closed |
| Invalid, duplicate-conflicting or negative `Content-Length`; `Content-Length` together with `Transfer-Encoding: chunked`; bad chunk framing | `400`, connection closed |
| Method longer than 7 characters | `400` |
| Connection silent for 60 s (mid-request) | `408`, then closed; an idle keep-alive connection is closed without a response |
| Header values over 255 characters, path params over 63, queries over 255 | truncated silently |
| No route for the path | `404` |
| Route exists for the path under another method | `405` with `Allow` |
| Response headers larger than 8 KiB | connection closed without a response |

Known behavior to be aware of: a request whose request line is not valid HTTP (no version, `HTTP/2.0`, plain garbage) currently gets **no response**; the connection stays open until 8 KiB arrive or the 60 s idle timeout fires. Pipelined requests (a second request sent before the first response) are not supported: only the first is answered. Both are tracked in [`lib/CLAUDE.md`](lib/CLAUDE.md) under "Known gaps".

---

## 8. API Reference Quick Index

The complete list, one line per function, is [`lib/API.md`](lib/API.md) (kept in sync by `make check-docs`). The essentials:

| Function | Header | Description |
|---|---|---|
| `app_init(App *app)` | `router.h` | Initializes an application instance and connection table. |
| `app_listen(App *app, int port)` | `connection.h` | Starts the server (delegates to cluster if `config.workers != 1`). |
| `app_destroy(App *app)` | `connection.h` | Releases connections, routes, event loop descriptors, and TLS state. |
| `app_on_worker_start(App *app, hook)` | `connection.h` | Registers a callback run once per worker process, after any fork. |
| `app_enable_tls(App *app, cert, key)` | `router.h` | Enables HTTPS with PEM files (TLS 1.2+). |
| `app_get(...)` / `app_post(...)` / `app_put(...)` / `app_patch(...)` / `app_delete(...)` | `router.h` | Registers verb routes (`app_head`, `app_options` override the automatic answers). |
| `app_get_mw(...)` etc. | `router.h` | Same, with per-route middleware. |
| `router_init(Router *)`, `router_get(...)` etc., `router_use(...)` | `router.h` | Builds a sub-router. |
| `app_mount(App *app, prefix, Router *sub)` | `router.h` | Mounts a sub-router under a path prefix. |
| `app_serve_static(App *app, prefix, root)` | `router.h` | Mounts a static file directory with traversal protection. |
| `app_use(App *app, Middleware mw)` | `middleware.h` | Registers an app-wide middleware. |
| `app_use_prefix(App *app, prefix, Middleware mw)` | `middleware.h` | Registers a prefix-scoped middleware. |
| `app_use_error(App *app, ErrorHandler eh)` | `middleware.h` | Registers the centralized error handler. |
| `chain_next(chain)` / `chain_error(chain, status, message)` | `middleware.h` | Continue the pipeline / fail the request. |
| `req_get_param(req, name)` | `router.h` | Extracts a path parameter by name. |
| `req_get_query(req, name)` / `req_get_header(req, name)` / `req_get_cookie(req, name)` | `http_parser.h` | Query, header (case-insensitive) and cookie lookups. |
| `res_status(res, code)` / `res_set_header(res, name, value)` | `response.h` | Status and custom headers. |
| `res_send(res, body)` / `res_json(res, json)` / `res_send_bytes(res, type, data, len)` | `response.h` | Sends a whole body (copied). |
| `res_redirect(res, status, location)` | `response.h` | Performs an HTTP redirect. |
| `res_set_cookie(res, name, val, opts)` / `res_clear_cookie(res, name, path)` | `response.h` | Cookies. |
| `res_write(res, data, len)` / `res_set_trailer(...)` / `res_end(res)` | `response.h` | Chunked streaming. |
| `res_send_file(res, type, filepath)` | `response.h` | Streams a file in bounded 16 KiB chunks. |
| `arena_yyjson_alc(Arena *)` | `arena.h` | yyjson allocator backed by the connection arena. |
| `parse_urlencoded_body(...)`, `multipart_parse_boundary(...)`, `parse_multipart_body(...)` | `urlencoded.h`, `multipart.h` | Form and upload parsers. |
| `cluster_resolve_worker_count(n)`, `cluster_is_worker()`, `cluster_worker_id()` | `cluster.h` | Cluster helpers. |
