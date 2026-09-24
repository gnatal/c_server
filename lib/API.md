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
- `app_listen(App *, int port)` — run until SIGINT/SIGTERM (cluster if `config.workers` != 1). Listens on `config.bind_address`: a numeric IPv4/IPv6 literal (`"127.0.0.1"` behind a same-host proxy, `"::"` = every IPv4 and IPv6 interface), `NULL` (default) = every IPv4 interface; a hostname or malformed address exits at startup. The string must outlive `app_listen`. `config.max_connections` (default 10,000 per worker; beyond it 503) and `config.max_buffered_bytes` (default 256 MiB per worker: memory held across event-loop turns by partial requests/uploads, unsent response tails and stream buffers; beyond it an upload gets 503 and a client that stops reading is closed; 0 = no budget).
- `app_destroy(App *)` — free everything `app_init` and route registration allocated; call after `app_listen` returns.
- `app_on_worker_start(App *, WorkerInitHook)` — run a hook once per serving process, after fork (open DB handles here).
- `app_listen_worker(App *, int port)`, `app_listen_cluster(App *, int port, int workers)`, `app_stop(App *)`, `app_count_connections(const App *)` — lower-level lifecycle.
- `app_listen_worker_via_control_socket(App *, int control_fd)` — cluster-internal (macOS/BSD `CEXPRESS_SINGLE_ACCEPTOR`): entry point for a worker that receives connections as fds passed by the master over `control_fd`, rather than binding and accepting itself. Not meant to be called by application code.
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
- `app_serve_static(App *, prefix, root_dir)` — serve files from a directory (traversal-safe; dotfiles are 404; `nosniff` on every answer; the root is resolved against the working directory).
- `app_use_body_limit(App *, prefix, max_bytes)` — 413 for a body over `max_bytes` on paths under `prefix` (matched on the canonical path, so `?query`, `//` and `%XX` don't dodge it; clamped to `MAX_BODY_SIZE`). A declared `Content-Length` is refused before buffering; a chunked body once its chunks decode past the limit. `app_body_limit_for_path(const App *, path)` / `app_body_limit_for_target(const App *, target, len)` — the effective cap for a canonical path / a raw request-target (engine/tests).
- `match_route(const App *, Request *)`, `match_path(pattern, path, Request *)`, `match_route_allowed_methods(...)` — matching (engine/tests).
- `app_free_routes(App *)` — free the route trees (engine: `app_destroy` calls it).
- Path rules that surprise: a literal segment beats `:name` beats `*` regardless of registration order; use the same `:name` at the same position in every route (`lib/CLAUDE.md`, "Known gaps").

## Middleware (`middleware.h`)
- `app_use(App *, Middleware)`, `app_use_prefix(App *, prefix, Middleware)` — app-wide middleware, optionally path-scoped (prefix normalized: `"/admin/"`, `"admin"` and `"/admin"` are the same; matched on segments against the canonical `req->path`).
- `app_use_error(App *, ErrorHandler)` — the one error sink for `chain_error`.
- `chain_next(MiddlewareChain *)` — continue the pipeline. `chain_error(MiddlewareChain *, status, message)` — fail the request.
- `dispatch(App *, const Route *, const Request *, Response *)` — run the pipeline for one request (engine/tests).

## Read the request (`http_parser.h`, `router.h`)
- `req_get_param(req, name)` — `:name` path parameter.
- `req_get_query(req, name)` — query value, decoded, case-sensitive name.
- `req_get_header(req, name)` — header value, case-insensitive name. No length limit of its own (views into the raw request, not fixed-size copies) other than the whole header block fitting `BUF_SIZE`.
- `req_get_cookie(req, name)` — cookie value, case-sensitive name. Splits the `Cookie` header on its first call per request, not eagerly for every request.
- Also on `Request`: `method`, `path`, `version`, `query` (raw), `body` (NUL-terminated; binary-safe with `content_length`; lives in the connection arena, never free it), `content_length`.
- `url_decode(src, dst, dst_size, decode_plus)` — percent-decode a string. `url_decode_span(src, len, dst, dst_size, decode_plus)` — same over a length, -2 if it does not fit.
- `request_target_path(target, len, out, out_size)` — raw request-target to canonical path (query dropped, decoded, `path_canonicalize`d; -2 too long, -4 invalid): the one step the parser and the body-limit check share.
- `path_canonicalize(path)` (in place, collapses repeated `/`; -1 for a `.`/`..` segment or a target not starting with `/`), `path_normalize_prefix(prefix, out, out_size)` (`""` or `/seg[/seg]`), `path_prefix_matches(prefix, path)` (segment-boundary match) — the one path shape shared by routing, prefix middleware, body limits and mounts. `req->path` is always canonical: the parser refuses (400) `%2F`, dot segments and non-origin-form targets.

## Build the response (`response.h`)
- `res_status(res, code)`, `res_set_header(res, name, value)` — before sending `Content-Length`, `Connection` and `Date` are engine-managed. Value copied whole (no length cap; the whole head must fit 8 KiB or the connection is dropped); name > 63 chars is dropped. Status 1xx/204/304 sends the head only (no framing headers, no body).
- `res_send(res, text)`, `res_json(res, json_text)`, `res_send_bytes(res, type, data, len)` — send a whole body (copied).
- `res_send_shared(res, type, SharedBody *)` — send a reference-counted body without copying it: the connection pins it (one reference) until written; you keep yours. `shared_body_new(len)` (refs 1, `NULL` on OOM), `shared_body_retain(b)`, `shared_body_release(b)` (frees at 0). The bytes must not change while referenced. The static-file cache sends every hit this way.
- `res_redirect(res, status, location)` — 3xx + Location (status 0 = 302). 500 if the Location has control chars or cannot be stored.
- `res_set_cookie(res, name, value, const CookieOptions *)`, `res_clear_cookie(res, name, path)` — cookies.
- `res_write(res, data, len)`, `res_end(res)`, `res_set_trailer(res, name, value)` — chunked response, buffered until the handler returns (at most `MAX_BODY_SIZE`, silently truncated past it); for big bodies use `res_stream`.
- `res_send_file(res, content_type, path)` — stream a file (kernel sendfile, no user-space copy; pread fallback); `0` ok, `-1` nothing sent.
- `res_stream(res, StreamProducer, ctx, StreamCtxFree)` — large or endless body (downloads, server-sent events): the event loop calls the producer each time the previous output has drained, so a connection holds at most `STREAM_CHUNK_SIZE`; it returns `STREAM_MORE` / `STREAM_PAUSE` / `STREAM_END` / `STREAM_ABORT`. On `0` the engine owns `ctx` and frees it exactly once; on `-1` you still own it. The producer must not touch `req`, `res` or the arena.
- `stream_write(StreamWriter *, data, len)` — inside a producer: append one chunk; `-1` (nothing written) when this turn is full: return `STREAM_MORE` and retry. At most `STREAM_WRITE_MAX` per call.
- `app_wake_streams(App *)` (`connection.h`) — resume every paused producer on this worker (e.g. from a publishing handler); paused producers are also resumed about once a second.
- `stream_release(Connection *)` — engine: detach a producer stream and free its ctx.
- `shared_body_detach(Connection *)` — engine: drop a connection's pinned shared body.
- `res_init(res, conn)` — engine/tests: prepare a Response.

## Per-request memory (`arena.h`)
- `arena_yyjson_alc(Arena *)` — a `yyjson_alc` that allocates from an arena; use `res->conn->arena` (a pointer to the shared per-worker arena - not `&res->conn->arena`). Documents built or read with it need no free; the arena is reclaimed after the response is written.
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
- `parse_urlencoded_body(body, len, UrlEncodedForm *)` → field count, -1 a decoded `%00`, -2 a name > 63 or value > 255 bytes (either: 400, form is empty), `urlencoded_get_field(form, name)`.
- `multipart_parse_boundary(content_type, out, out_size)` → 1/0, `parse_multipart_body(body, len, boundary, MultipartForm *)` → part count or -1, `multipart_get_part(form, name)`, `multipart_safe_filename(part, out, out_size)` → 1/0 (basename, no control chars, never `.`/`..`: the only form of `part->filename` safe for a filesystem - the raw field is exactly what the client sent).

## Parser internals (`http_parser.h`) — engine and tests
- `parse_http_request(raw, raw_len, Request *, Arena *)`, `request_is_complete(buf, len)`, `request_framing(buf, len, &header_len, &chunked, &path, &path_len)` (`path`/`path_len` out params optional, pass `NULL`), `request_wants_close(req)`.
- `parse_request_head(buf, len, ParsedHead *)`, `parse_request_head_resume(buf, len, ParsedHead *, size_t *head_scan)` (same result; the end-of-head blank-line search resumes from `*head_scan` as the same request's buffer grows, and picohttpparser runs only once a blank line is in - linear, not quadratic, for a head that trickles in; `connection.c` carries `Connection.head_scan`), `request_head_is_complete(ParsedHead *, buf, len, ChunkScanState *)` (last arg resumes a chunked-body scan across reads of one request, `NULL` = from scratch), `request_wire_len(ParsedHead *, ChunkScanState *)` (headers + framed body length of a complete request, i.e. where a pipelined next request starts), `request_head_expects_continue(ParsedHead *)` (1 when an HTTP/1.1+ head with a body to wait for carries `Expect: 100-continue`; `connection.c` then writes `100 Continue` once per request), `parse_http_request_from_head(raw, raw_len, ParsedHead *, Request *, Arena *)` (one `phr_parse_request` pass, reused by `connection.c`'s body-limit check, completeness check and full parse instead of each running its own; `request_framing`/`request_is_complete`/`parse_http_request` are thin wrappers over these and unchanged in behavior), `parse_http_request_in_place(char *raw, raw_len, ParsedHead *, Request *, Arena *, char *saved_byte_out)` (the engine's path - `req->body` points into `raw` instead of being copied, chunked bodies decoded in place; writes one NUL at `body[content_length]` and returns the byte it replaced for the caller to restore).
- `extract_content_length(block)`, `request_has_chunked_encoding(block)`, `chunked_body_scan(...)`, `chunked_body_scan_resume(body, avail, max, ChunkScanState *, &decoded_len)` (continues from and advances the state; same codes), `chunked_body_decode(...)`.
- `parse_query_string(query, req)`, `parse_headers(block, req, Arena *)`, `parse_cookies(value, req)`, `status_text(code)`, `format_http_date(t, out)` (pure IMF-fixdate), `http_date_for(now)` (per-second cached `Date` value).

## Engine internals — do not call from app code
- Connections (`connection.h`): `set_nonblocking`, `create_server_socket(bind_address, port)` (the listener every path binds - `app_listen_worker`, the cluster preflight and the macOS single acceptor; `NULL` = `0.0.0.0`, otherwise a strict `inet_pton` literal, `IPV6_V6ONLY` off so `::` also takes IPv4; -1 on failure; listener is non-blocking with `TCP_NODELAY`, both inherited by accepted sockets), `accept_client(listen_fd)` (one syscall per connection - accept4 with SOCK_NONBLOCK | SOCK_CLOEXEC on Linux, plain accept on BSD/macOS; used by `accept_connections` and the single-acceptor cluster master), `connection_create`, `connection_close`, `accept_connections`, `accept_passed_connections` (macOS/BSD `CEXPRESS_SINGLE_ACCEPTOR` only — fd-passing counterpart of `accept_connections`), `handle_readable` (serves every pipelined request in the buffer, in order, up to `MAX_PIPELINED_PER_EVENT` per event; reads into the per-worker `App.read_buf` when nothing is buffered and copies only unserved bytes into a connection-owned `in_buf`), `handle_writable` (the write-readiness handler - drains a pending response, then resumes buffered pipelined requests), `flush_connection` (returns `FLUSH_DONE` / `FLUSH_PENDING` / `FLUSH_CLOSED`; on keep-alive advances `conn->in_off` past `conn->request_len` instead of discarding `in_buf`), `close_idle_connections`.
- Event loop (`event_loop.h`; kqueue on macOS/BSD; on Linux epoll, or io_uring with `CEXPRESS_EVENT_LOOP=io_uring` (no fallback: init fails if the ring is refused), `NO_URING=1` to build epoll only): `event_loop_init`, `event_loop_close`, `event_loop_is_open`, `event_loop_backend_name` ("kqueue" / "io_uring" / "epoll" / "none"), `event_loop_watch_read`, `event_loop_unwatch_read`, `event_loop_watch_write`, `event_loop_unwatch_write`, `event_loop_release_fd`, `event_loop_arm_shutdown_timer`, `event_loop_poll`.
- Cluster (`cluster.h`): `cluster_listen`, `cluster_resolve_worker_count`, `cluster_is_worker`, `cluster_worker_id`.
- Static files (`static.h`): `static_serve_file`, `static_resolve_relative_path`, `static_mime_type`, `static_path_hash` (FNV-1a 64 of a path; the cache compares it before `strcmp`), `static_cache_clear` (clears `static_serve_file`'s in-memory file cache; tests and app-triggered reloads only, nothing in the engine calls it).
