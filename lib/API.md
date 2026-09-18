# lib/ public API index

One line per function, grouped by task. Semantics, limits and ownership are in the header comments
(`lib/*.h`); worked, tested examples are in `lib/examples/cookbook.c`. Include everything with
`#include "cexpress.h"`. `make check-docs` fails if this file and the headers disagree.

Conventions: functions return `0`/`-1` (or a count) unless noted; `NULL` from an accessor means
"absent"; strings you pass in are copied; pointers you get back belong to the object they came from
(see "Ownership" in `lib/CLAUDE.md`).

## Start a server (`connection.h`, `router.h`)
- `app_init(App *)` — reset an App and allocate its connection table. `App` is ~48 KB: use `static` or main's stack.
- `app_listen(App *, int port)` — run until SIGINT/SIGTERM (cluster if `config.workers` != 1).
- `app_destroy(App *)` — free everything `app_init` allocated; call after `app_listen` returns.
- `app_on_worker_start(App *, WorkerInitHook)` — run a hook once per serving process, after fork (open DB handles here).
- `app_enable_tls(App *, cert_pem, key_pem)` — enable HTTPS; `0` ok, `-1` bad args.
- `app_listen_worker(App *, int port)`, `app_listen_cluster(App *, int port, int workers)`, `app_stop(App *)`, `app_count_connections(const App *)` — lower-level lifecycle.

## Routes (`router.h`)
- `app_get` / `app_post` / `app_put` / `app_patch` / `app_delete` / `app_head` / `app_options` `(App *, path, Handler)` — register a route.
- `app_get_mw` / `app_post_mw` / `app_put_mw` / `app_patch_mw` / `app_delete_mw` / `app_head_mw` / `app_options_mw` `(App *, path, Handler, const Middleware *, int count)` — same with per-route middleware.
- `app_add_route(App *, method, path, Handler)`, `app_add_route_mw(...)` — generic forms.
- `router_init(Router *)`, `router_use(Router *, Middleware)` — create a sub-router; `router_use` adds router-level middleware.
- `router_get` / `router_post` / `router_put` / `router_patch` / `router_delete` / `router_head` / `router_options` `(Router *, path, Handler)` — register on the router.
- `router_get_mw` / `router_post_mw` / `router_put_mw` / `router_patch_mw` / `router_delete_mw` / `router_head_mw` / `router_options_mw` `(Router *, path, Handler, const Middleware *, int count)` — same with per-route middleware.
- `router_add_route(Router *, method, path, Handler)`, `router_add_route_mw(...)` — generic forms.
- `app_mount(App *, prefix, const Router *)` — copy a router's routes and middleware into the app under a prefix.
- `app_serve_static(App *, prefix, root_dir)` — serve files from a directory (traversal-safe).
- `match_route(const App *, Request *)`, `match_path(pattern, path, Request *)`, `match_route_allowed_methods(...)` — matching (engine/tests).

## Middleware (`middleware.h`)
- `app_use(App *, Middleware)`, `app_use_prefix(App *, prefix, Middleware)` — app-wide middleware, optionally path-scoped.
- `app_use_error(App *, ErrorHandler)` — the one error sink for `chain_error`.
- `chain_next(MiddlewareChain *)` — continue the pipeline. `chain_error(MiddlewareChain *, status, message)` — fail the request.
- `dispatch(App *, const Route *, const Request *, Response *)` — run the pipeline for one request (engine/tests).

## Read the request (`http_parser.h`, `router.h`)
- `req_get_param(req, name)` — `:name` path parameter.
- `req_get_query(req, name)` — query value, decoded, case-sensitive name.
- `req_get_header(req, name)` — header value, case-insensitive name.
- `req_get_cookie(req, name)` — cookie value, case-sensitive name.
- Also on `Request`: `method`, `path`, `version`, `query` (raw), `body` (NUL-terminated; binary-safe with `content_length`), `content_length`.
- `url_decode(src, dst, dst_size, decode_plus)` — percent-decode a string.

## Build the response (`response.h`)
- `res_status(res, code)`, `res_set_header(res, name, value)` — before sending.
- `res_send(res, text)`, `res_json(res, json_text)`, `res_send_bytes(res, type, data, len)` — send a whole body.
- `res_redirect(res, status, location)` — 3xx + Location (status 0 = 302).
- `res_set_cookie(res, name, value, const CookieOptions *)`, `res_clear_cookie(res, name, path)` — cookies.
- `res_write(res, data, len)`, `res_end(res)`, `res_set_trailer(res, name, value)` — chunked streaming.
- `res_send_file(res, content_type, path)` — stream a file; `0` ok, `-1` nothing sent.
- `res_init(res, conn)` — engine/tests: prepare a Response.

## JSON output (`json/json.h`) — prefer JsonWriter
- `jw_init(JsonWriter *)`, `jw_free(JsonWriter *)` — lifecycle; always `jw_free`.
- `jw_object_begin` / `jw_object_end` / `jw_array_begin` / `jw_array_end` — containers.
- `jw_key(w, key)` — object member name; every object value needs one first.
- `jw_string(w, text)` (NULL → null), `jw_int(w, long long)`, `jw_double(w, double)`, `jw_bool(w, int)`, `jw_null(w)` — values.
- `jw_ok(w)`, `jw_data(w)`, `jw_len(w)` — result; `jw_data` is NULL unless the document is complete and valid.

## JSON input and trees (`json/json.h`)
- `json_parse(text, err, err_size)` → `JsonValue *` or NULL; `json_free(root)`.
- `json_object_get(obj, key)`, `json_array_get(arr, i)`, `json_array_count(arr)`, `json_is_null(v)` — navigate (NULL-safe).
- `json_as_string(v, default)`, `json_as_number(v, default)`, `json_as_bool(v, default)` — typed reads; strings point into the tree.
- `json_stringify(v)` → malloc'd string that the caller frees with the C library `free`.
- `json_new_string` / `json_new_number` / `json_new_bool` / `json_new_object` / `json_new_array`, `json_object_set(obj, key, value)`, `json_array_append(arr, value)` — tree builders; the last two take ownership of `value` even on failure.

## Forms and uploads (`urlencoded.h`, `multipart.h`)
- `parse_urlencoded_body(body, len, UrlEncodedForm *)`, `urlencoded_get_field(form, name)`.
- `multipart_parse_boundary(content_type, out, out_size)` → 1/0, `parse_multipart_body(body, len, boundary, MultipartForm *)` → part count or -1, `multipart_get_part(form, name)`.

## Parser internals (`http_parser.h`) — engine and tests
- `parse_http_request(raw, raw_len, Request *)`, `request_is_complete(buf, len)`, `request_framing(buf, len, &header_len, &chunked)`, `request_wants_close(req)`.
- `extract_content_length(block)`, `request_has_chunked_encoding(block)`, `chunked_body_scan(...)`, `chunked_body_decode(...)`.
- `parse_query_string(query, req)`, `parse_headers(block, req)`, `parse_cookies(value, req)`, `status_text(code)`.

## Engine internals — do not call from app code
- Connections (`connection.h`): `set_nonblocking`, `create_server_socket`, `connection_create`, `connection_close`, `accept_connections`, `handle_readable`, `flush_connection`, `close_idle_connections`.
- Event loop (`event_loop.h`, kqueue or epoll): `event_loop_init`, `event_loop_close`, `event_loop_watch_read`, `event_loop_unwatch_read`, `event_loop_watch_write`, `event_loop_unwatch_write`, `event_loop_unwatch_all`, `event_loop_arm_shutdown_timer`, `event_loop_poll`.
- Cluster (`cluster.h`): `cluster_listen`, `cluster_resolve_worker_count`, `cluster_is_worker`, `cluster_worker_id`.
- TLS (`tls.h`): `tls_is_available`, `tls_init_app`, `tls_cleanup_app`, `tls_connection_init`, `tls_connection_handshake`, `tls_connection_read`, `tls_connection_write`, `tls_connection_close`, `tls_has_pending`.
- Static files (`static.h`): `static_serve_file`, `static_resolve_relative_path`, `static_mime_type`.
