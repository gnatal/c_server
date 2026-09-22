# lib/ public API index

One line per function, grouped by task. Semantics, limits and ownership are in the header comments
(`lib/*.h`); worked, tested examples are in `lib/examples/cookbook.c`. Include everything with
`#include "cexpress.h"` (it also pulls in the vendored yyjson header). `make check-docs` fails if this file and the
`lib/*.h` headers disagree; the yyjson subset listed below is only checked for names that do not exist.

Conventions: functions return `0`/`-1` (or a count) unless noted; `NULL` from an accessor means
"absent"; strings you pass in are copied; pointers you get back belong to the object they came from
and, for anything on a `Request` or `Response`, die when the handler returns (see "Memory model" and
"Ownership" in `lib/CLAUDE.md`).

## Start a server (`connection.h`, `router.h`)
- `app_init(App *)` — reset an App and allocate its connection table. `App` is small (about 4.6 KB on macOS; routes are heap-allocated); a `Router` is about 88 KB on macOS (64 route slots), so prefer `static` for it.
- `app_listen(App *, int port)` — run until SIGINT/SIGTERM (cluster if `config.workers` != 1).
- `app_destroy(App *)` — free everything `app_init` and route registration allocated; call after `app_listen` returns.
- `app_on_worker_start(App *, WorkerInitHook)` — run a hook once per serving process, after fork (open DB handles here).
- `app_listen_worker(App *, int port)`, `app_listen_cluster(App *, int port, int workers)`, `app_stop(App *)`, `app_count_connections(const App *)` — lower-level lifecycle.
- No TLS: this engine is plaintext HTTP/1.1 only. Terminate TLS at a gateway or reverse proxy in front of it.

## Routes (`router.h`)
- `app_get` / `app_post` / `app_put` / `app_patch` / `app_delete` / `app_head` / `app_options` `(App *, path, Handler)` — register a route.
- `app_get_mw` / `app_post_mw` / `app_put_mw` / `app_patch_mw` / `app_delete_mw` / `app_head_mw` / `app_options_mw` `(App *, path, Handler, const Middleware *, int count)` — same with per-route middleware.
- `app_add_route(App *, method, path, Handler)`, `app_add_route_mw(...)` — generic forms.
- `router_init(Router *)`, `router_use(Router *, Middleware)` — create a sub-router; `router_use` adds router-level middleware.
- `router_get` / `router_post` / `router_put` / `router_patch` / `router_delete` / `router_head` / `router_options` `(Router *, path, Handler)` — register on the router.
- `router_get_mw` / `router_post_mw` / `router_put_mw` / `router_patch_mw` / `router_delete_mw` / `router_head_mw` / `router_options_mw` `(Router *, path, Handler, const Middleware *, int count)` — same with per-route middleware.
- `router_add_route(Router *, method, path, Handler)`, `router_add_route_mw(...)` — generic forms.
- `app_mount(App *, prefix, const Router *)` — copy a router's routes and middleware into the app under a prefix.
- `app_serve_static(App *, prefix, root_dir)` — serve files from a directory (traversal-safe; the root is resolved against the working directory).
- `app_use_body_limit(App *, prefix, max_bytes)` — reject a declared `Content-Length` over `max_bytes` for paths under `prefix` with 413, before buffering the body (clamped to `MAX_BODY_SIZE`; chunked bodies unaffected). `app_body_limit_for_path(const App *, path)` — the effective cap for a path (engine/tests).
- `match_route(const App *, Request *)`, `match_path(pattern, path, Request *)`, `match_route_allowed_methods(...)` — matching (engine/tests).
- `app_free_routes(App *)` — free the route trees (engine: `app_destroy` calls it).
- Path rules that surprise: a literal segment beats `:name` beats `*` regardless of registration order; use the same `:name` at the same position in every route (`lib/CLAUDE.md`, "Known gaps").

## Middleware (`middleware.h`)
- `app_use(App *, Middleware)`, `app_use_prefix(App *, prefix, Middleware)` — app-wide middleware, optionally path-scoped.
- `app_use_error(App *, ErrorHandler)` — the one error sink for `chain_error`.
- `chain_next(MiddlewareChain *)` — continue the pipeline. `chain_error(MiddlewareChain *, status, message)` — fail the request.
- `dispatch(App *, const Route *, const Request *, Response *)` — run the pipeline for one request (engine/tests).

## Read the request (`http_parser.h`, `router.h`)
- `req_get_param(req, name)` — `:name` path parameter.
- `req_get_query(req, name)` — query value, decoded, case-sensitive name.
- `req_get_header(req, name)` — header value, case-insensitive name. No length limit of its own (P3: views into the raw request, not fixed-size copies) other than the whole header block fitting `BUF_SIZE`.
- `req_get_cookie(req, name)` — cookie value, case-sensitive name. Splits the `Cookie` header on its first call per request (P3), not eagerly for every request.
- Also on `Request`: `method`, `path`, `version`, `query` (raw), `body` (NUL-terminated; binary-safe with `content_length`; lives in the connection arena, never free it), `content_length`.
- `url_decode(src, dst, dst_size, decode_plus)` — percent-decode a string.

## Build the response (`response.h`)
- `res_status(res, code)`, `res_set_header(res, name, value)` — before sending.
- `res_send(res, text)`, `res_json(res, json_text)`, `res_send_bytes(res, type, data, len)` — send a whole body (copied).
- `res_redirect(res, status, location)` — 3xx + Location (status 0 = 302).
- `res_set_cookie(res, name, value, const CookieOptions *)`, `res_clear_cookie(res, name, path)` — cookies.
- `res_write(res, data, len)`, `res_end(res)`, `res_set_trailer(res, name, value)` — chunked streaming.
- `res_send_file(res, content_type, path)` — stream a file; `0` ok, `-1` nothing sent.
- `res_init(res, conn)` — engine/tests: prepare a Response.

## Per-request memory (`arena.h`)
- `arena_yyjson_alc(Arena *)` — a `yyjson_alc` that allocates from an arena; use `res->conn->arena` (a pointer to the shared per-worker arena, M1 - not `&res->conn->arena`). Documents built or read with it need no free; the arena is reclaimed after the response is written.
- `arena_alloc(Arena *, size)` — bump-allocate 8-byte-aligned bytes, valid until the request ends (falls back to `malloc` when the 64 KiB buffer is full; `NULL` only on OOM). Handlers may use it for scratch data.
- `arena_init(Arena *, buf, cap)`, `arena_reset(Arena *)`, `arena_destroy(Arena *)` — engine/tests: lifecycle. Tests give a fake `Connection` a static buffer with `arena_init`.

## JSON (yyjson 0.13, `vendor/yyjson/yyjson.h`) — the subset this repo uses
Full reference: <https://ibireme.github.io/yyjson/doc/doxygen/html/>. Pattern: build with a document that uses the arena,
serialize once, `free` the string.
- Write: `yyjson_mut_doc_new(&alc)` (`alc` from `arena_yyjson_alc`), `yyjson_mut_obj(doc)`, `yyjson_mut_arr(doc)`, `yyjson_mut_doc_set_root(doc, root)`.
- Add members: `yyjson_mut_obj_add_str(doc, obj, key, value)`, `yyjson_mut_obj_add_int(doc, obj, key, value)`, `yyjson_mut_obj_add_bool(doc, obj, key, value)`, `yyjson_mut_arr_append(arr, val)`. Keys and string values are borrowed, not copied: they must outlive the write (the `yyjson_mut_obj_add_strcpy` family copies).
- Serialize: `yyjson_mut_write(doc, 0, &len)` → NUL-terminated string from **libc malloc**, always freed by the caller with `free`, even when the doc uses the arena. `NULL` on failure. Then `yyjson_mut_doc_free(doc)` (a no-op for an arena doc, required for a `NULL`-allocator doc).
- Read: `yyjson_read_opts(body, len, 0, &alc, NULL)` → `yyjson_doc *` or `NULL` (flags `0` copies the input, so `req->body` is untouched). `yyjson_doc_get_root(doc)`, then `yyjson_obj_get(obj, key)`, `yyjson_arr_size(arr)`, `yyjson_arr_get(arr, i)`, `yyjson_get_str(v)`, `yyjson_get_sint(v)`, `yyjson_get_bool(v)`. Getters return `NULL` / `0` on a missing or wrong-typed value. Strings point into the doc and die with the arena (or `yyjson_doc_free` for a `NULL`-allocator doc).
- Finish with `yyjson_doc_free(doc)` for reads.

## Forms and uploads (`urlencoded.h`, `multipart.h`)
- `parse_urlencoded_body(body, len, UrlEncodedForm *)`, `urlencoded_get_field(form, name)`.
- `multipart_parse_boundary(content_type, out, out_size)` → 1/0, `parse_multipart_body(body, len, boundary, MultipartForm *)` → part count or -1, `multipart_get_part(form, name)`.

## Parser internals (`http_parser.h`) — engine and tests
- `parse_http_request(raw, raw_len, Request *, Arena *)`, `request_is_complete(buf, len)`, `request_framing(buf, len, &header_len, &chunked, &path, &path_len)` (`path`/`path_len` out params optional, pass `NULL`), `request_wants_close(req)`.
- `parse_request_head(buf, len, ParsedHead *)`, `request_head_is_complete(ParsedHead *, buf, len)`, `parse_http_request_from_head(raw, raw_len, ParsedHead *, Request *, Arena *)` (P2: one `phr_parse_request` pass, reused by `connection.c`'s body-limit check, completeness check and full parse instead of each running its own; `request_framing`/`request_is_complete`/`parse_http_request` are thin wrappers over these and unchanged in behavior).
- `extract_content_length(block)`, `request_has_chunked_encoding(block)`, `chunked_body_scan(...)`, `chunked_body_decode(...)`.
- `parse_query_string(query, req)`, `parse_headers(block, req, Arena *)`, `parse_cookies(value, req)`, `status_text(code)`.

## Engine internals — do not call from app code
- Connections (`connection.h`): `set_nonblocking`, `create_server_socket`, `connection_create`, `connection_close`, `accept_connections`, `handle_readable`, `flush_connection`, `close_idle_connections`.
- Event loop (`event_loop.h`; kqueue on macOS/BSD, io_uring on Linux, epoll behind `CEXPRESS_USE_EPOLL`): `event_loop_init`, `event_loop_close`, `event_loop_watch_read`, `event_loop_unwatch_read`, `event_loop_watch_write`, `event_loop_unwatch_write`, `event_loop_unwatch_all`, `event_loop_arm_shutdown_timer`, `event_loop_poll`.
- Cluster (`cluster.h`): `cluster_listen`, `cluster_resolve_worker_count`, `cluster_is_worker`, `cluster_worker_id`.
- Static files (`static.h`): `static_serve_file`, `static_resolve_relative_path`, `static_mime_type`, `static_cache_clear` (P1: clears `static_serve_file`'s in-memory file cache; tests and app-triggered reloads only, nothing in the engine calls it).
