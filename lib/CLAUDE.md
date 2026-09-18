# lib/ — CExpress engine

Read order for a new task: this file → `API.md` (every public function) → `examples/cookbook.c`
(tested recipes) → the header of the module you touch. `app/` is a full worked application.

## Model
A process runs one single-threaded, non-blocking event loop (kqueue on macOS/BSD, epoll on Linux).
`workers != 1` forks N such processes sharing the port via `SO_REUSEPORT`; a master respawns any that
die. Handlers run synchronously on the loop: a blocking call (DB, sleep) stalls that whole worker, so
scale with workers, not threads. State is per process; there is no shared memory.

Per request (`connection.c: handle_readable`):
1. `recv` into `conn->in_buf` until `request_is_complete` (`http_parser.c`, header-scoped, no body scan).
2. `parse_http_request` → `Request` on the stack. Failure → reject (400 / 413 / 414), close.
3. `keep_alive = !request_wants_close && !shutting_down`; `res.is_head_request` set.
4. `match_route` (linear, first match, fills `:params`) → `dispatch` (`middleware.c`): app-wide middleware
   (prefix-filtered) → route middleware → handler, or the default 404 / 405 / OPTIONS answer, or a static file.
5. The handler calls `res_*` (`response.c`), which only builds bytes into `conn->out_buf`.
6. `free(req.body)`; `flush_connection` writes; keep-alive resets the connection, otherwise closes.

Only `connection.c`, `event_loop_*.c`, `tls.c`, `cluster.c` do I/O. Parsing, routing, dispatch and
response building never touch a socket, so tests drive them with a fake `Connection` (see
`tests/test_cookbook.c: fetch`). Keep it that way.

## Files
| File | Responsibility |
|---|---|
| `app_types.h` | every struct/typedef and every compile-time limit |
| `http_parser.c/h` | request parsing, framing (Content-Length / chunked), accessors, `status_text` |
| `router.c/h` | route tables, `match_path`, sub-routers (`app_mount`), `app_serve_static`, `app_enable_tls` |
| `middleware.c/h` | pipeline (`chain_next`, `chain_error`, `dispatch`), 404/405/OPTIONS defaults |
| `response.c/h` | response head assembly, cookies, chunked streaming, file streaming |
| `connection.c/h` | accept, read/parse/dispatch/flush, buffer growth, idle timeout, shutdown, listen |
| `event_loop.h` + `event_loop_kqueue.c` / `event_loop_epoll.c` | one API over two backends (fds, timers, signals) |
| `cluster.c/h` | fork workers, respawn, drain |
| `tls.c/h` | non-blocking OpenSSL; stubs when built with `NO_TLS=1` |
| `static.c/h` | traversal-safe file serving |
| `multipart.c/h`, `urlencoded.c/h` | form body parsers (handler-invoked, not automatic) |
| `json/` | `json_parse` tree, `JsonWriter` (`jw_*`) emitter, tree builders (see `json/CLAUDE.md`) |
| `examples/cookbook.c` | tested few-shot recipes; `tests/test_cookbook.c` runs every one |

## Limits (all compile-time, in `app_types.h`; excess is truncated or dropped, never overflowed)
Routes 32 · app middleware 16 · route middleware 8 · path params 8 (value 63) · query params 16 (63) ·
request headers 32 (value 255) · cookies 16 (255) · request headers total 8 KiB (`BUF_SIZE`, else 431) ·
path 255 (else 414) · query 255 · body 10 MiB (`MAX_BODY_SIZE`, else 413) · response headers 16 ·
Set-Cookie 16 (512 each) · trailers 8 · multipart parts 16 · form fields 32 · static file 50 MiB ·
idle timeout 60 s · drain deadline 5 s.

## Ownership (who frees what)
| Thing | Allocated by | Freed by |
|---|---|---|
| `Request.body` | `parse_http_request` (always non-NULL after success) | the engine, after dispatch. Handlers never free it |
| `req_get_*` results, `MultipartPart.data` | point inside the Request / body | nobody; valid until the handler returns |
| `conn->out_buf` | `res_*` (one malloc per response; a second send frees the first) | `flush_connection` or `connection_close` |
| `conn->in_buf`, `Connection` | `connection_create` (+ realloc on growth) | `connection_close` (exactly once) |
| `app->connections` | `app_init` | `app_destroy` |
| `JsonValue` tree | `json_parse`, `json_new_*` | `json_free(root)` only. `json_object_set` / `json_array_append` take ownership of `value` even on failure |
| `json_as_string` result | inside the tree | dies with `json_free` |
| `json_stringify` result | `json_stringify` | caller, C `free` |
| `JsonWriter` buffer | `jw_*` | `jw_free` (always call; idempotent) |
| `SSL`, `SSL_CTX` | `tls_connection_init`, `tls_init_app` | `tls_connection_close`, `tls_cleanup_app` |

## Return conventions
`0` ok / `-1` error for setup functions (`app_enable_tls`, `res_send_file`, `event_loop_*`, `create_*`).
`parse_http_request`: `0` ok, `-1` malformed, `-2` path too long (→ 414); after `-1`, `req.content_length == -2` means body too large (→ 413).
`request_is_complete`: `1` also for invalid framing (stop reading, let the parser report it). `chunked_body_scan`: `1` done, `0` need more, `-1` malformed, `-2` too large.
`tls_connection_handshake`: `1` done, `0` in progress, `-1` fatal. `tls_connection_read/write`: bytes, `0` EOF, `-1` with `errno` (`EAGAIN` = wait).
`json_object_set`, `json_array_append`: `1` ok, `0` failed. `JsonWriter`: failure is sticky, check `jw_ok` once at the end.
Accessors return `NULL` for "absent". Nothing in the engine uses exceptions or `errno` for logic outside the socket layer.

## Behavior reference (non-obvious rules; the code is the spec for the rest)
- **Routing.** Deny by default. Exact method match; no path match → 404; path matches another method → 405 + `Allow`.
  `HEAD` falls back to the same path's `GET` route (body suppressed, `Content-Length` kept). `OPTIONS` on a known path
  → 200 + `Allow` (with `HEAD` added if `GET` exists). Explicit `app_head` / `app_options` win. Segments split on `/`,
  empty ones ignored; `:name` captures; a middle `*` matches one segment; a trailing `*` matches one or more (not the bare prefix).
- **Middleware.** Order = registration order (app-wide, only entries whose prefix matches at a segment boundary; runs for 404s
  too) → route middleware → handler. One error handler (`app_use_error`, last wins); default is `res_status` + `res_send`.
  Handlers get no chain and cannot call `chain_next` / `chain_error`. Code after `chain_next` sees the final `res->status`.
- **Sub-routers.** `app_mount` copies routes (prefix prepended; `/` mounts at the bare prefix) and turns `router_use`
  middleware into prefix-scoped app middleware. The Router may be a stack local. No nesting.
- **Static.** `app_serve_static` registers `GET <prefix>/*`, `realpath`s the root once, refuses `..` (403), re-checks the
  resolved path stays under the root after symlink resolution (403), 404 for non-files, serves `index.html` for a directory,
  never lists. Reads the file into memory (≤ 50 MiB) and sends it with `res_send_bytes`.
- **Request parsing.** Header names match exactly and case-insensitively at line start (never by substring). `Content-Length`
  must be plain digits; duplicates must agree; `Content-Length` together with chunked is 400 (smuggling shape). Method ≤ 7
  chars. Path percent-decoded before routing (so `%2F` becomes a segment break). Query/headers/cookies parsed eagerly.
  Chunked bodies: extensions ignored, trailers discarded, decoded size capped at `MAX_BODY_SIZE`, raw wire size capped at
  `header_len + MAX_BODY_SIZE`.
- **Buffers.** `in_buf` starts at 8 KiB; with headers complete and a body pending it is realloc'd once to the exact size
  (chunked: doubling to the cap) and shrunk back when the connection goes idle. No header terminator within 8 KiB → 431.
- **Response safety.** Header names/values, trailers and cookie fields containing control characters are dropped
  (response-splitting defense); `res_redirect` with such a target answers 500. `Content-Length` and `Connection` are engine-owned.
  `CookieOptions` zero value = session cookie; `max_age > 0` seconds, `< 0` expire now.
- **Timeouts.** `last_activity` advances on received bytes only. Sweep every second; ≥ 60 s silent → close (408 first if a
  request was half-received). A connection with a response pending is never timed out (slow readers are not this timeout's job).
- **Shutdown.** SIGINT/SIGTERM (kqueue `EVFILT_SIGNAL` / `signalfd`, no async handlers) → `app_stop`: stop accepting, close idle
  connections, in-flight ones get `Connection: close`, 5 s deadline; a second signal exits at once. Cluster master forwards
  SIGTERM, waits 6 s, then SIGKILLs.
- **Workers and fork.** Never open a database or socket in `main()` before `app_listen`; register `app_on_worker_start`
  and open there (runs once per serving process, after fork). `SIGPIPE` is ignored per process in `app_listen_worker`.
- **TLS.** `app_enable_tls` (or `TLS_CERT` / `TLS_KEY` in `app/main.c`). TLS 1.2+, non-blocking handshake driven by the loop,
  `SSL_pending` checked so pipelined bytes buffered inside OpenSSL are not stranded. `make NO_TLS=1` removes the dependency.
- **Event loop.** `Connection.events_watched` mirrors what the kernel has registered on both backends; `watch_*` / `unwatch_*`
  skip the syscall when the state already matches (a keep-alive response costs no extra `kevent` / `epoll_ctl`).

## Hot-path rules (measured; do not undo)
Per-request CPU cost of the pure path (parse, route, dispatch, response build; no sockets, one core), measured on the same
benchmark before and after: minimal GET 1.37 → 0.39 µs, browser-shaped GET (10 headers, cookies, query) 3.9 → 0.80 µs,
JSON POST 1.6 → 0.47 µs, 404 1.6 → 0.43 µs. End to end with `wrk` (4 workers) that was +10% on `GET /` and +110% on
`GET /api/todos` (JSON tree → `JsonWriter`). `make bench` prints current numbers; only ratios transfer between machines.
What made the difference, so what not to reintroduce:
- No `strtok_r` / `sscanf` / `strncpy` (zero-pads to the full size) / `strcasestr` over request bytes. Scan with lengths and `memchr`.
- No whole-struct `memset` of `Request` (19 KB) or `Response` (16 KB). `parse_http_request` and `res_init` set scalars and
  `*_count` only; arrays are read up to their count and every slot is NUL-terminated on write.
- `match_path` walks pattern and path in place; `req == NULL` matches without capturing (no scratch `Request` copy for 404/405).
- Response head is assembled with bounded `memcpy` appends and an integer formatter, not `snprintf`.
- Emit JSON with `JsonWriter`: about 4x faster than building a tree and calling `json_stringify` (`make bench`).
- No syscall on a path that changes nothing (`events_watched`).
Verification tools: `make bench`, `make test` (15 suites), `make SANITIZE=1 BUILD_DIR=build-asan test` (ASan + UBSan),
`make fuzz` (mutation fuzzer over parser/router/response), `make check-docs`. Run sanitizers and fuzz after touching
`http_parser.c`, `router.c`, `response.c` or `json_writer.c`. The Makefile tracks header dependencies (`-MMD`).

## Known gaps (verified, not fixed)
- **Pipelining is dropped.** After the first request is answered, `flush_connection` sets `in_len = 0`, discarding any further
  request already in the buffer (client sees one response for two requests). Fix needs the parser to report bytes consumed,
  `memmove` of the remainder, and re-running the parse loop after each flush.
- **Chunked request bodies are re-scanned from the start on every `recv`** (`request_is_complete` → `chunked_body_scan`):
  quadratic in the worst case up to 10 MiB, so a slow-drip client can burn CPU. Fix: keep scan position and decoded length per connection.
- **Each accepted connection allocates an 8 KiB `in_buf`** up front (5000 idle connections ≈ 40 MiB).
- **`Request.body` is a malloc'd copy** per request (including a 1-byte allocation for empty bodies); a zero-copy body would need NUL-termination handling.
- Path `%2F` decodes before segmenting; queries over 255 chars, header values over 255 and params over 63 are truncated silently.
- No HTTP/2, `Expect: 100-continue`, compression, `Range`, or WebSocket. Routes match linearly (≤ 32).

## Where to change what
Add a response helper → `response.c/h` + `tests/test_response.c` + `API.md`. Add a parser feature → `http_parser.c/h` +
`tests/test_http_parser.c` (or `test_http_hardening.c` for a bug regression) + a case in `tests/fuzz_parser.c` seeds. Add a route feature → `router.c/h` + `tests/test_router.c`.
Add middleware behavior → `middleware.c` + `tests/test_middleware.c`. New public function → declare it in the header and list it in
`API.md` (`make check-docs` enforces this). New recipe → `examples/cookbook.c` + `tests/test_cookbook.c`.
