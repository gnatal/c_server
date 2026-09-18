# CExpress API & Developer Documentation

Welcome to the **CExpress** documentation. CExpress brings the developer ergonomics and modular architecture of [Express.js](https://expressjs.com/) to native C (C11), powered by non-blocking `kqueue` (macOS/BSD) and `epoll` (Linux) event-driven I/O.

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
5. [Advanced Features](#5-advanced-features)
   - [Streaming & Chunked Responses](#streaming--chunked-responses)
   - [Chunked Trailers](#chunked-trailers)
   - [Bounded File Streaming (`res_send_file`)](#bounded-file-streaming-res_send_file)
   - [Static File Serving (`app_serve_static`)](#static-file-serving-app_serve_static)
   - [Multipart & Form Data Parsing](#multipart--form-data-parsing)
   - [Built-in JSON Engine](#built-in-json-engine)
   - [Graceful Shutdown](#graceful-shutdown)
6. [API Reference Quick Index](#6-api-reference-quick-index)

---

## 1. Getting Started

### Including the Umbrella Header
CExpress provides a single umbrella header:

```c
#include "cexpress.h"
```

This includes all core subsystems: routing, response helpers, middleware chains, HTTP parser, static files, multipart, URL-encoded forms, and JSON utilities.

### Compiling & Linking
Compile your application files and link against `libcexpress.a`:

```bash
gcc -Wall -Wextra -std=c11 -O2 -Ipath/to/cexpress/lib -o my_app main.c path/to/cexpress/build/lib/libcexpress.a
```

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

    /* Cleans up remaining connections and event loop resources upon shutdown */
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
- **`OPTIONS` Requests**: Automatically supported. Returns a `204 No Content` with the appropriate `Allow` header matching registered methods.
- **`405 Method Not Allowed`**: If a path matches but the method does not, CExpress returns `405` with the `Allow` header.

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

### Wildcard Routes (`*`)
A trailing `*` segment captures the remainder of the path:

```c
app_get(&app, "/files/*", handler_files);

void handler_files(const Request *req, Response *res) {
    /* req->path contains the full path, e.g. "/files/docs/readme.txt" */
    res_send(res, req->path);
}
```

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
Runs for every incoming request:

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
Runs only for requests matching a URL prefix:

```c
/* Only executes for requests starting with "/admin" */
app_use_prefix(&app, "/admin", mw_admin_auth);
```

### Per-Route Middleware
Attach middleware specifically to a single route:

```c
app_post_mw(&app, "/checkout", handler_checkout, (Middleware[]){mw_require_auth, mw_rate_limit}, 2);
```

Sub-routers also support router-level middleware:
```c
router_use(&api_router, mw_require_auth);
```

### Centralized Error Handling
Trigger centralized error processing with `chain_error`:

```c
void mw_guard(const Request *req, Response *res, MiddlewareChain *chain) {
    if (!authorized) {
        chain_error(chain, 403, "Access Forbidden");
        return;
    }
    chain_next(chain);
}
```

Register a custom error handler:
```c
void my_error_handler(int status, const char *message, const Request *req, Response *res) {
    (void)req;
    res_status(res, status);
    char json[256];
    snprintf(json, sizeof(json), "{\"status\":%d,\"error\":\"%s\"}", status, message);
    res_json(res, json);
}

app_use_error(&app, my_error_handler);
```

---

## 4. Request & Response API

### The `Request` Object
The `Request` struct (`req`) provides read-only request metadata:
- `req->method`: HTTP verb (`"GET"`, `"POST"`, `"PUT"`, etc.).
- `req->path`: Clean URL path (e.g. `"/users"`).
- `req->query`: Raw query string (e.g. `"sort=asc&limit=10"`).
- `req->body`: Request body bytes (`NULL` if no body).
- `req->content_length`: Body length in bytes.

#### Helpers
- `req_get_param(req, "paramName")`: Lookup named route variable.
- `req_get_query(req, "queryKey")`: Lookup query parameter by key.
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

#### Custom Headers & Status
```c
res_status(res, 201);
res_set_header(res, "X-Server-Name", "CExpress-Edge");
```
*(Reserved headers `Content-Length` and `Connection` are managed automatically by the response layer).*

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

---

## 5. Advanced Features

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

### Chunked Trailers
Attach trailers (e.g. `Server-Timing`, checksums) to chunked responses:

```c
res_set_trailer(res, "Server-Timing", "db;dur=14.2, render;dur=3.1");
res_write(res, "data", 4);
res_end(res);
```

### Bounded File Streaming (`res_send_file`)
Stream files directly from disk through the event loop in 16KB bounded chunks without buffering entire files into RAM:

```c
void handler_download(const Request *req, Response *res) {
    (void)req;
    if (res_send_file(res, "application/pdf", "reports/annual.pdf") != 0) {
        res_status(res, 404);
        res_send(res, "File not found");
    }
}
```

### Static File Serving (`app_serve_static`)
Mount an entire directory of public assets with automatic MIME-type detection, path traversal defense, and `index.html` fallback:

```c
/* GET /static/style.css -> serves app/public/style.css */
app_serve_static(&app, "/static", "app/public");
```

### Multipart & Form Data Parsing

#### URL-Encoded Forms (`application/x-www-form-urlencoded`)
```c
UrlEncodedForm form;
parse_urlencoded_body(req->body, req->content_length, &form);

for (int i = 0; i < form.field_count; i++) {
    printf("%s = %s\n", form.field_names[i], form.field_values[i]);
}
```

#### Multipart Uploads (`multipart/form-data`)
```c
const char *content_type = req_get_header(req, "Content-Type");
MultipartForm form;
parse_multipart_body(content_type, req->body, req->content_length, &form);

for (int i = 0; i < form.part_count; i++) {
    MultipartPart *part = &form.parts[i];
    if (part->is_file) {
        printf("Uploaded file: %s (%zu bytes)\n", part->filename, part->data_len);
    }
}
```

### Built-in JSON Engine
Parse and construct JSON ASTs without external dependencies:

```c
char err[128];
JsonValue *root = json_parse(req->body, err, sizeof(err));
if (root != NULL) {
    const JsonValue *title = json_object_get(root, "title");
    if (title && title->type == JSON_STRING) {
        printf("Title: %s\n", title->string_val);
    }
    json_free(root);
}
```

Construct JSON:
```c
JsonValue *obj = json_new_object();
json_object_set(obj, "status", json_new_string("healthy"));
char *json_str = json_stringify(obj);

res_json(res, json_str);

free(json_str);
json_free(obj);
```

### Multi-Worker Concurrency (`SO_REUSEPORT`)
CExpress scales linearly across CPU cores using a multi-process worker model powered by kernel-level `SO_REUSEPORT` socket load balancing.
1. Master process manages worker lifecycle, tracks PIDs, and intercepts termination signals.
2. Each worker opens an independent listening socket with `SO_REUSEPORT` on the same port and runs an isolated event loop.
3. Master automatically detects crashed workers and respawns replacement workers.
4. Graceful cluster shutdown coordinates draining across all workers within a 5-second deadline.
5. See [concurrency.md](concurrency.md) for full architecture details and container guidelines.

### Graceful Shutdown
CExpress intercepts `SIGINT` (`Ctrl+C`) and `SIGTERM` directly in the native event loop (`EVFILT_SIGNAL` on kqueue, `signalfd` on Linux):
1. Stops accepting incoming TCP connections immediately.
2. Closes idle keep-alive connections.
3. Drains in-flight requests and file streams.
4. Triggers a 5-second deadline timer before force-exiting if clients stall.
5. Returns cleanly from `app_listen` to allow `app_destroy` to release all memory.

---

## 6. API Reference Quick Index

| Function | File | Description |
|---|---|---|
| `app_init(App *app)` | `router.h` | Initializes an application instance and connection table. |
| `app_listen(App *app, int port)` | `connection.h` | Starts the server (delegates to cluster if `config.workers > 1`). |
| `app_listen_worker(App *app, int port)` | `connection.h` | Runs the single-process event loop directly. |
| `app_listen_cluster(App *app, port, n)` | `connection.h` | Explicitly launches a multi-process cluster of `n` workers. |
| `cluster_listen(App *app, port, n)` | `cluster.h` | Master supervisor coordinating `n` worker processes. |
| `cluster_resolve_worker_count(n)` | `cluster.h` | Resolves worker count (auto-detects CPU cores if `n <= 0`). |
| `cluster_is_worker()` | `cluster.h` | Returns 1 if running inside a cluster worker process. |
| `cluster_worker_id()` | `cluster.h` | Returns 0-indexed worker ID or -1 if master. |
| `app_stop(App *app)` | `connection.h` | Initiates graceful shutdown and connection draining. |
| `app_destroy(App *app)` | `connection.h` | Releases connections table, event loop descriptors, and resources. |
| `app_get(...)` / `app_post(...)` | `router.h` | Registers verb routes. |
| `app_put(...)` / `app_patch(...)` / `app_delete(...)` | `router.h` | Registers PUT, PATCH, and DELETE routes. |
| `app_mount(App *app, prefix, Router *sub)` | `router.h` | Mounts a sub-router under a path prefix. |
| `app_serve_static(App *app, prefix, root)` | `router.h` | Mounts a static file directory with traversal protection. |
| `app_use(App *app, Middleware mw)` | `router.h` | Registers an app-wide middleware. |
| `app_use_prefix(App *app, prefix, Middleware mw)` | `router.h` | Registers a prefix-scoped middleware. |
| `app_use_error(App *app, ErrorHandler eh)` | `router.h` | Registers centralized error handler. |
| `chain_next(MiddlewareChain *chain)` | `middleware.h` | Advances to the next middleware or route handler. |
| `chain_error(chain, status, message)` | `middleware.h` | Passes error to centralized error handler. |
| `req_get_param(req, name)` | `http_parser.h` | Extracts a path parameter by name. |
| `req_get_query(req, name)` | `http_parser.h` | Extracts a query string parameter by name. |
| `req_get_header(req, name)` | `http_parser.h` | Looks up a request header (case-insensitive). |
| `req_get_cookie(req, name)` | `http_parser.h` | Looks up a cookie value by name. |
| `res_status(res, status_code)` | `response.h` | Sets HTTP response status. |
| `res_set_header(res, name, value)` | `response.h` | Sets a custom response header. |
| `res_send(res, body_str)` | `response.h` | Sends a `text/plain` body. |
| `res_json(res, json_str)` | `response.h` | Sends an `application/json` body. |
| `res_send_bytes(res, type, data, len)` | `response.h` | Sends raw binary data. |
| `res_redirect(res, status, location)` | `response.h` | Performs HTTP redirect. |
| `res_set_cookie(res, name, val, opts)` | `response.h` | Sets a `Set-Cookie` header with options. |
| `res_clear_cookie(res, name, path)` | `response.h` | Expires a cookie immediately. |
| `res_write(res, data, len)` | `response.h` | Emits a chunk in a chunked response. |
| `res_set_trailer(res, name, val)` | `response.h` | Attaches a chunked trailer header. |
| `res_end(res)` | `response.h` | Finalizes a chunked response. |
| `res_send_file(res, type, filepath)` | `response.h` | Streams a file in bounded 16KB chunks. |
