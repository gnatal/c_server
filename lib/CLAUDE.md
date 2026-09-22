# lib/ — CExpress engine

Read order for a new task: this file → `API.md` (every public function) → `examples/cookbook.c`
(tested recipes) → the header of the module you touch. `examples/todo_sqlite/` is a full worked application.
`../tradeoffs.md` explains why the arena, yyjson, picohttpparser and the Patricia router exist.

**No TLS.** This engine speaks plaintext HTTP/1.1 only. TLS termination belongs at a gateway or reverse
proxy in front of it (nginx, an ALB, a sidecar), not inside a library whose job is parsing HTTP/1.1 -
mixing the two coupled OpenSSL's lifecycle (handshake state, buffer release, renegotiation hardening) into
every connection's state machine for a concern the engine has no business owning. An OpenSSL-based
non-blocking TLS layer (`tls.c/h`, `TLS_CERT`/`TLS_KEY` in the demo) existed through 2026-09-22 and was
removed for this reason; see `../improvements_progress.md` for the removal record.

## Model
A process runs one single-threaded, non-blocking event loop: kqueue on macOS/BSD, io_uring on Linux
(readiness only: multishot `POLL_ADD` on each fd, then ordinary `recv`/`write`; needs liburing and a kernel with
multishot poll, 5.13+). An epoll backend (`event_loop_epoll.c`) is kept for `-DCEXPRESS_USE_EPOLL`; the Makefile does not select it
on Linux, and on macOS it is only built for `make test_epoll` through epoll-shim. `workers != 1` forks N such processes sharing
the port via `SO_REUSEPORT`; a master respawns any that die, with backoff and a restart budget (S7, see "Behavior
reference, Workers and fork"). Handlers run synchronously on the loop: a blocking call (DB,
sleep) stalls that whole worker, so scale with workers, not threads. State is per process; there is no shared memory.
If `event_loop_init` fails (for example io_uring is blocked by the runtime), `app_listen_worker` prints the error and exits; there is no runtime fallback to epoll.

Per request (`connection.c: handle_readable`):
1. `recv` into `conn->in_buf` until `request_is_complete` (`http_parser.c`: runs picohttpparser over the headers, plus a chunked scan when the body is chunked).
2. `parse_http_request(in_buf, in_len, &req, &conn->arena)` → `Request` on the stack (copies method/path/headers/cookies into its fixed arrays; the body is copied into the connection arena). Failure → reject (400 / 413 / 414 / 431), close.
3. `keep_alive = !request_wants_close && !shutting_down`; `res.is_head_request` set.
4. `match_route` (per-method Patricia tree, fills `:params`) → `dispatch` (`middleware.c`): app-wide middleware
   (prefix-filtered) → route middleware → handler, or the default 404 / 405 / OPTIONS answer, or a static file.
5. The handler calls `res_*` (`response.c`), which only builds bytes into `conn->out_buf` (allocated from the arena).
6. `flush_connection` writes. Keep-alive: `arena_reset`, `in_len = 0`, shrink `in_buf`. Otherwise `connection_close`.

Only `connection.c`, `event_loop_*.c`, `cluster.c` do I/O. Parsing, routing, dispatch and
response building never touch a socket, so tests drive them with a fake `Connection` whose arena is a static buffer
(see `tests/test_cookbook.c: fetch`). Keep it that way.

## Files
| File | Responsibility |
|---|---|
| `app_types.h` | every struct/typedef and every compile-time limit |
| `arena.c/h` | per-connection bump allocator (`arena_alloc`, `arena_reset`, `arena_yyjson_alc`) |
| `http_parser.c/h` | request parsing on top of picohttpparser, framing (Content-Length / chunked), accessors, `status_text` |
| `router.c/h` | route registration, one Patricia (segment-radix) tree per method, `app_mount`, `app_serve_static`, `app_free_routes` |
| `middleware.c/h` | pipeline (`chain_next`, `chain_error`, `dispatch`), 404/405/OPTIONS defaults |
| `response.c/h` | response head assembly, cookies, chunked streaming, file streaming |
| `connection.c/h` | accept, read/parse/dispatch/flush, buffer growth, idle timeout, shutdown, listen |
| `event_loop.h` + `event_loop_kqueue.c` / `event_loop_io_uring.c` / `event_loop_epoll.c` | one API over three backends (fds, timers, signals) |
| `cluster.c/h` | fork workers, respawn (with backoff and a restart budget, S7), drain |
| `static.c/h` | traversal-safe file serving, with an in-memory cache of recently served files (P1) |
| `multipart.c/h`, `urlencoded.c/h` | form body parsers (handler-invoked, not automatic) |
| `vendor/picohttpparser/` | vendored HTTP/1.x request parser (MIT/Perl) |
| `vendor/yyjson/` | vendored yyjson 0.13.0; JSON reading and writing. `cexpress.h` includes it. There is no engine JSON layer of its own |
| `examples/cookbook.c` | tested few-shot recipes; `tests/test_cookbook.c` runs every one |

## Limits (all compile-time, in `app_types.h`; excess is truncated or dropped, never overflowed, except where marked)
Routes: no fixed cap per App (each is malloc'd into a tree), 64 per Router (`MAX_ROUTER_ROUTES`), 16 distinct methods · app middleware 16 · route middleware 8 ·
path params 8 (value 63) · query params 16 (63) · request headers 32 (name 63, value `MAX_HEADER_VALUE_LEN` 1024; **a 33rd
header is a 400, a name or value that doesn't fit is a 431 (S5), neither is a silent drop or truncation**) ·
cookies 16 (255) · request headers total 8 KiB (`BUF_SIZE`, else 431) · path 255 (else 414) · query 255 ·
body 10 MiB (`MAX_BODY_SIZE`, else 413) · response headers 16 · Set-Cookie 16 (512 each) · trailers 8 · multipart parts 16 ·
form fields 32 · static file 50 MiB · static file cache 256 entries, 256 KiB each, 64 MiB total, 1 s revalidation
(`STATIC_CACHE_*`, `static.c`; a file over the per-entry cap is served but never cached; see "Static" below - P1) ·
idle timeout 60 s · request header deadline 10 s · request body deadline 30 s ·
pending-response write-stall deadline 30 s · drain deadline 5 s · response head 8 KiB (larger: the connection is closed without a response) ·
worker init hooks 4 · cluster workers 128 · arena 64 KiB per connection (see below; exceeding it falls back to malloc, it is not a limit) ·
max connections 10,000 per worker (`ServerConfig.max_connections`, `DEFAULT_MAX_CONNECTIONS`; a runtime config field, not a compile-time-only limit like the others here - `0` opts out, uncapped) ·
body limit prefixes 16 (`MAX_BODY_LIMITS`; `App.body_limits`, set at runtime by `app_use_body_limit`, unlike the other limits here - see "Body limits (S4)" below).

## Memory model
Every accepted connection is one `calloc(sizeof(Connection) + 64 KiB)`: the arena buffer sits right behind the struct
(`connection_create`). `in_buf` is a separate 8 KiB malloc. Measured on macOS, 5,000 idle keep-alive connections on one worker took
about 121 MB RSS, about 25 KB per connection (Linux not measured). The arena serves everything that lives for one request:
`Request.body`, `conn->out_buf`, the file-streaming chunk buffer, chunked-response growth, and any yyjson document created
with `arena_yyjson_alc`. Bump allocation, 8-byte aligned, no per-allocation free. When the remaining space is too small
(not only for a single request over 64 KiB), the allocation falls back to `malloc` and is chained in a list that `arena_reset` frees.
`flush_connection` calls `arena_reset` when a keep-alive response is fully written; `connection_close` calls `arena_destroy`.
Consequences: nothing reached through `req` or `res` may be kept past the handler; a growing chunked response copies into a new arena block each doubling
and leaves the old block in the arena until the request ends; a static file is read into a malloc'd buffer and copied again into the arena by `res_send_bytes`.

## Ownership (who frees what)
| Thing | Allocated by | Freed by |
|---|---|---|
| `Connection` + its 64 KiB arena buffer | `connection_create` (one calloc) | `connection_close` (exactly once) |
| `conn->in_buf` | `connection_create` (+ realloc on growth) | `connection_close` |
| Arena fallback blocks | `arena_alloc` when the buffer is full | `arena_reset` (each keep-alive response) or `arena_destroy` (close) |
| `Request.body` | `parse_http_request`, from the arena (always non-NULL after success) | nobody: reclaimed with the arena. Handlers never free it |
| `req_get_*` results, `MultipartPart.data` | point inside the Request / body | nobody; valid until the handler returns |
| `conn->out_buf` | `res_*`, from the arena (one allocation per response; a second send just leaves the first in the arena) | nobody: the pointer is dropped by `flush_connection` / `connection_close` |
| yyjson doc built or read with `arena_yyjson_alc(&conn->arena)` | arena | nothing: `yyjson_*_doc_free` is a no-op for it, the arena reclaims it |
| yyjson doc with a NULL allocator (e.g. `error_handler_json`) | libc malloc | `yyjson_mut_doc_free` / `yyjson_doc_free` |
| `yyjson_mut_write(doc, 0, &len)` result | libc malloc, **whatever allocator the doc uses** | caller, C `free` (forgetting it leaks once per request) |
| `Route`, `PatriciaNode` | `app_add_route_mw`, `app_serve_static`, `tree_insert` | `app_free_routes`, called by `app_destroy` |
| `app->connections` | `app_init` | `app_destroy` |
| `app->spare_fd` (S3, one `/dev/null` fd held in reserve for `EMFILE`) | `app_init` | `app_destroy`; also closed-then-reopened across its life by `accept_connections` (on `EMFILE`) and `connection_close` (opportunistic re-arm) - see Behavior reference, Overload |
| Static file cache entries (P1: cached path string + file bytes, `static.c`'s own process-lifetime global, not tied to any `App`) | `static_serve_file`, on a cache miss or a changed file | replaced in place on the next change, evicted (stalest first) once `STATIC_CACHE_MAX_ENTRIES` is reached, or all of them via `static_cache_clear` (tests; nothing in the engine calls it) |

## Return conventions
`0` ok / `-1` error for setup functions (`res_send_file`, `event_loop_*`, `create_*`).
`parse_http_request`: `0` ok, `-1` malformed, `-2` path too long (→ 414), `-3` a header name or value too long to store (→ 431, S5), `-4` a percent-decoded path/query name/query value contains an embedded NUL (→ 400, S6); after `-1`, `req.content_length == -2` means body too large (→ 413).
`url_decode` / `parse_query_string`: `0` ok, `-1` a decoded byte was NUL (S6) - the destination is still fully written and NUL-terminated, but the caller must treat it as invalid input rather than use it.
`request_is_complete`: `1` for a complete request and also for invalid `Content-Length` / chunked+`Content-Length` framing (stop reading, let the parser report it);
`0` while more bytes are needed, **and also (known gap, below) when the request line or headers are malformed**. `chunked_body_scan`: `1` done, `0` need more, `-1` malformed, `-2` too large.
yyjson: read functions return `NULL` on failure; `yyjson_mut_*_add_*` return `false` on failure (the cookbook and demo do not check them).
Accessors return `NULL` for "absent". Nothing in the engine uses exceptions or `errno` for logic outside the socket layer.

## Behavior reference (non-obvious rules; the code is the spec for the rest)
- **Routing.** Deny by default. Exact method match; no path match → 404; path matches another method → 405 + `Allow`.
  Each method has its own Patricia tree keyed by path segments (split on `/`, empty segments ignored, so `/a//b/` = `/a/b`). At each node the
  search tries, in this order and with backtracking: a literal child, then the `:name` / mid-pattern `*` child, then a trailing `*`.
  So **specificity beats registration order**: `/users/me` wins over `/users/:id` even when registered second.
  A mid-pattern `*` matches one segment and captures nothing. A trailing `*` matches one or more segments (never the bare prefix). A duplicate pattern for the same method keeps the first and warns.
  `HEAD` falls back to the same path's `GET` route (body suppressed, `Content-Length` kept). `OPTIONS` on a known path
  → 200 + `Allow` (with `HEAD` added if `GET` exists). Explicit `app_head` / `app_options` win. Path is percent-decoded before routing (so `%2F` becomes a segment break).
  `match_path` is a standalone pattern matcher that the router itself no longer calls; it is kept for tests and does not reproduce every tree rule (it is first-match, per pattern).
- **Middleware.** Order = registration order (app-wide, only entries whose prefix matches at a segment boundary; runs for 404s
  too) → route middleware → handler. One error handler (`app_use_error`, last wins); default is `res_status` + `res_send`.
  Handlers get no chain and cannot call `chain_next` / `chain_error`. Code after `chain_next` sees the final `res->status`.
- **Sub-routers.** `app_mount` copies routes (prefix prepended; `/` mounts at the bare prefix) and turns `router_use`
  middleware into prefix-scoped app middleware. The Router may be a stack local. No nesting.
- **Static.** `app_serve_static` registers `GET <prefix>/*`, `realpath`s the root once (a missing root registers nothing and logs it, so every request under the prefix is a 404), refuses `..` (403), re-checks the
  resolved path stays under the root after symlink resolution (403), 404 for non-files, serves `index.html` for a directory,
  never lists. Reads the whole file into memory (≤ 50 MiB) and sends it with `res_send_bytes`. The root is resolved against the process's working directory.
  **File cache (P1).** `static_serve_file` caches files up to `STATIC_CACHE_MAX_ENTRY_BYTES` after their first read, keyed by
  the pre-`realpath` candidate path (`static_root` + the already-traversal-checked subpath), not the resolved one - MEASURED
  (`improvements.md`, P1) a 6.6x gap between the static-mount path and an equivalent in-memory response, almost entirely
  `open`/`fstat`/`realpath`/`fopen`/`fread`/`malloc`/`free` paid on every request for the same handful of files. A request
  within `STATIC_CACHE_REVALIDATE_SECONDS` (1 s) of the same candidate's last check is served straight from the cache with
  **no filesystem call at all**, not even `realpath`/`stat` - re-verified live at 255,941 req/s for a 52-byte file
  (`wrk -t4 -c100 -d8s`, matching the in-memory `res_send_bytes` baseline in `improvements.md`, up from the 37,647 req/s
  measured there before this fix). Past that window, one `stat` compares size and mtime; unchanged reuses the cached bytes
  (skips `fopen`/`fread`), changed re-reads and replaces the entry. **Trade-off, by design:** a symlink swapped in place, or
  a file rewritten with the same size and the same one-second mtime, can serve stale content for up to the revalidation
  window - the same trade every `stat`-based file cache (e.g. nginx's `open_file_cache`) makes. A file over the per-entry cap
  is served normally but never cached (no behavior change for large files). The cache is a single process-lifetime table
  shared by every mount, not scoped to an `App` - see Ownership above and `static_cache_clear` (`static.h`) for the one thing
  that frees it (tests; not called anywhere in the engine itself).
- **Request parsing.** picohttpparser does the request line and header block; it is strict about tokens and accepts bare `\n` line endings, and rejects HTTP versions other than 1.x. `Content-Length`
  must be plain digits; duplicates must agree; `Content-Length` together with chunked is 400 (smuggling shape). Header names are matched exactly and case-insensitively (never by substring). Method ≤ 7
  chars. Query/headers/cookies parsed eagerly into fixed arrays. More than 32 headers → 400. A header name over 63 chars or
  value over `MAX_HEADER_VALUE_LEN` (1024) is rejected with 431, never silently truncated (S5: raised from the original
  255-char value cap, which used to truncate a Bearer JWT or long cookie into a value that compared unequal to itself with
  no indication why - `parse_headers`, the standalone component parser `tests/` uses directly and that the live request
  path does *not* call, still truncates silently since it has no error path to signal through, a `void` function).
  Chunked bodies: extensions ignored, trailers discarded, decoded size capped at `MAX_BODY_SIZE`, raw wire size capped at `header_len + MAX_BODY_SIZE`.
  **Embedded NUL (S6).** The path and query names/values are percent-decoded (`decode_bounded`/`url_decode`); a decoded byte
  that is NUL (`%00`, or a raw NUL byte already in the request line) is rejected with 400 (`parse_http_request`'s `-4`)
  rather than silently truncating everything downstream that reads `req->path`/`req_get_query` as a C string - MEASURED
  (`improvements.md`, S6) `GET /static/style.css%00.png` used to be routed and served as `/static/style.css`, a bypass for
  any suffix/extension check performed on the path before use. Header and cookie values are not percent-decoded by this
  engine at all, so this vector does not apply to them (`req_get_header`/`req_get_cookie` already return raw bytes;
  header/cookie length limits are the separate S5 concern above).
- **Buffers.** `in_buf` starts at 8 KiB; with headers complete and a body pending it grows by doubling, capped at the
  known target size (S4: `Content-Length` and chunked both work this way now - `Content-Length` used to realloc straight
  to `header_len + content_length + 1` in one step, reserving virtual memory proportional to what the client merely
  *declared* rather than what it had actually sent), and shrunk back when the connection goes idle. No header
  terminator within 8 KiB → 431.
- **Response safety.** Header names/values, trailers and cookie fields containing control characters are dropped
  (response-splitting defense); `res_redirect` with such a target answers 500. `Content-Length` and `Connection` are engine-owned.
  `CookieOptions` zero value = session cookie; `max_age > 0` seconds, `< 0` expire now.
- **Overload (S3).** `accept_connections` sheds load on two independent axes, both O(1), checked before
  `ensure_connection_capacity`/`connection_create` so an already-overloaded worker doesn't pay for either. (1)
  `App.open_connections` (a running count, `++` in `accept_connections`, `--` in `connection_close` - not
  `app_count_connections`, an O(n) rescan used only at shutdown) checked against `ServerConfig.max_connections`
  (`0` = uncapped): past it, the fd is still `accept()`ed (has to be, to answer at all) but gets a hand-built 503 +
  `Connection: close` and is closed immediately - no `Connection`, arena or event-loop registration. (2)
  `App.spare_fd`, one descriptor opened at `app_init` and held
  in reserve: on `accept()` failing with `EMFILE`/`ENFILE` (the process is out of descriptors, not just over
  `max_connections`), it's closed to free one slot and `accept_connections` retries. MEASURED (macOS/BSD): the
  connection that triggered *that* failure is already gone by then - the kernel dequeues and destroys it rather
  than leaving it in the backlog for the retry to recover, so there is no 503 for it specifically. What the freed
  slot does deliver is recovery: the *next* `accept()` (now or on a later call) succeeds instead of the worker
  staying stuck at the limit indefinitely. `spare_fd` stays consumed (`-1`) until `connection_close` opportunistically
  reopens it the moment anything closes (the cheapest point to retry, rather than waiting for the next accept batch).
  Unrelated to either: `create_server_socket`'s `listen()` backlog is `max(BACKLOG, SOMAXCONN)`, not the bare
  `BACKLOG` constant.
- **Body limits (S4).** `app_use_body_limit(app, prefix, max_bytes)` (`router.c`) registers a `BodyLimitEntry` in
  `App.body_limits` (same segment-boundary prefix match as app-wide middleware; `max_bytes` clamped down to
  `MAX_BODY_SIZE`, never loosened past it). `connection.c`'s `reject_if_over_body_limit`, called from
  `handle_readable` right after every `recv` (before `request_is_complete`), runs `request_framing` once per
  request (`Connection.body_limit_checked` guards repeat calls, cleared with `request_started` in
  `flush_connection`'s keep-alive branch) and, as soon as headers are complete, compares a declared
  `Content-Length` against `app_body_limit_for_path` (longest matching prefix wins, independent of registration
  order; `MAX_BODY_SIZE` if nothing matches) - over it is 413, sent before a single body byte is buffered or
  `in_buf` is grown. Only `Content-Length` is covered; chunked bodies stay governed by the global `MAX_BODY_SIZE`
  raw-wire cap in `grow_in_buf`/`chunked_body_scan` only (a deliberate scope decision, not a gap: chunked's raw-cap
  doubling was already proportional to bytes received, which is what S4 was chiefly about for `Content-Length`).
- **Timeouts.** `last_activity` advances on received bytes only. Sweep every second; ≥ 60 s silent → close (408 first if a
  request was half-received). A connection with a response pending (`out_buf != NULL` or `file_fd >= 0`) is exempt from
  this particular check — that axis is bounded separately, below.
  A second, independent clock, `Connection.request_started`, bounds a request's *total* time regardless of how often a byte
  arrives (a client sending one byte every few seconds keeps `last_activity` fresh forever, so the check above alone never
  fires): armed at the first byte of a request (`handle_readable`), cleared back to 0 when a keep-alive response is fully
  queued (`flush_connection`). A
  freshly accepted, still-silent connection (nothing sent yet at all) is unaffected and stays on the `last_activity` check
  alone, same as before this existed. The
  sweep applies `REQUEST_HEADER_TIMEOUT_SECONDS` while headers are still incomplete (checked with a throwaway
  `request_framing` call — negligible, it runs once per second per pending connection, not on the per-byte path) or the more
  generous `REQUEST_BODY_TIMEOUT_SECONDS` once they are complete and only the body is pending; either expiring closes with
  408 (or a bare close if nothing was received yet). A connection idle *between* requests
  (`request_started == 0`) is governed only by the first, `last_activity`-based check.
  A third clock, `Connection.last_write_progress`, bounds a *pending response* that is making no progress at all: armed by
  `flush_connection` the first time it runs for a given response, and advanced only when `write()` actually
  accepts bytes (not merely because `flush_connection` ran — an `EAGAIN` alone doesn't count as progress), reset to 0 once a
  keep-alive response is fully queued. The sweep closes (no response, nothing left to say) any connection with a response
  still pending whose `last_write_progress` is `≥ WRITE_TIMEOUT_SECONDS` old — a client that stopped reading, as opposed to
  one still slowly draining a large response (which keeps advancing the clock and is left alone, however long that takes in
  total).
- **Shutdown.** SIGINT/SIGTERM (kqueue `EVFILT_SIGNAL` / `signalfd`, no async handlers) → `app_stop`: stop accepting, close idle
  connections, in-flight ones get `Connection: close`, 5 s deadline; a second signal exits at once. Cluster master forwards
  SIGTERM, waits 6 s, then SIGKILLs.
- **Workers and fork.** Never open a database or socket in `main()` before `app_listen`; register `app_on_worker_start`
  and open there (runs once per serving process, after fork). `SIGPIPE` is ignored per process in `app_listen_worker`.
  **Respawn (S7).** Before forking anyone, `cluster_listen` calls `create_server_socket(port)` once itself and closes
  the fd - this is purely a validation call: `create_server_socket` already `perror`s and `exit()`s (S12: still
  unfixed there) on a bind/listen failure, so a fatal, permanent misconfiguration (the port already taken, permission
  denied on a privileged port, ...) now stops the master with one clear message instead of forking `workers_count`
  children that would all fail the exact same way - MEASURED (`improvements.md`, S7) 10,594 respawns and 31,788 log
  lines in about 4 seconds with `WORKERS=2` and the port already taken. A worker that exits abnormally *after*
  startup (a real crash, not a bind failure) is respawned with exponential backoff per slot (100 ms, doubling, capped
  at 30 s) instead of instantly; if a slot fails more than `CLUSTER_RESTART_BUDGET` (5) times within
  `CLUSTER_RESTART_WINDOW_MS` (60 s) - a sliding window, not a lifetime count, so an occasional unrelated crash over a
  long-running server's life doesn't eventually trip it - the master gives up on the whole cluster, drains whatever
  workers are still up the same way a SIGTERM would, and `exit()`s non-zero itself (same S12 caveat: no error code
  returned to `app_listen`/`main()`). Both mechanisms are implementation details of `cluster.c` (the constants above
  are file-local, not in `app_types.h`, same convention as `connection.c`'s `ARENA_SIZE`) and share one accounting
  helper, `record_worker_failure`, so a `fork()` failure while trying to (re)spawn a slot counts against the same
  budget as an abnormal exit rather than looping unbounded on its own.
- **Event loop.** `Connection.events_watched` mirrors what the loop has registered. Only the kqueue backend uses it to skip the syscall when the state already
  matches, so a keep-alive response costs no extra `kevent` there. The epoll and io_uring backends issue a syscall on every `watch_*` / `unwatch_*`
  (`epoll_ctl`; io_uring submits a poll-remove plus a new multishot poll), including the `unwatch_write` that `flush_connection` runs after every keep-alive response.
  `LOOP_EVENT_ERROR` (POLLERR/POLLHUP or a negative completion) closes the connection.

## Hot-path rules (measured; do not undo)
Per-request CPU cost of the pure path (parse, route, dispatch, response build; no sockets, one core; `make bench`, Apple M3 Pro,
gcc-16 -O2, 22 Sep 2026): minimal GET 198 ns, browser-shaped GET (10 headers, cookies, query) 825 ns, JSON POST 313 ns, 404 208 ns;
a 20-row JSON list through yyjson 709 ns. (Previously 208 / 781 / 315 / 186 / 659 ns on 21 Sep 2026, before S5 raised
`Request.header_values`' per-slot size from 256 to `MAX_HEADER_VALUE_LEN` (1024, see "Behavior reference, Request parsing"):
the deltas above are within ordinary single-run noise, not attributable to that change - `copy_bounded` copies only
`headers[i].value_len` bytes actually present, never the destination array's capacity, so a bigger unused slot costs nothing
per request; only `sizeof(Request)` grew, not its CPU cost.) The earlier version of this file recorded 390 / 800 / 470 / 430 ns
for the handwritten-parser engine on the same machine (no A/B rebuild of that commit was done for this update); only ratios
transfer between machines. What to keep:
- No `strtok_r` / `sscanf` / `strncpy` (zero-pads to the full size) / `strcasestr` over request bytes. Scan with lengths and `memchr`.
- No whole-struct `memset` of `Request` (43,576 bytes, `sizeof`, `make bench`; grew from 19 KB when S5 raised the per-header
  value cap - see above) or `Response` (16 KB). `parse_http_request` and `res_init` set scalars and
  `*_count` only; arrays are read up to their count and every slot is NUL-terminated on write.
- Routing is one tree walk over path segments with no allocation; `req == NULL` searches without capturing (used for the 405 `Allow` list).
- Response head is assembled with bounded `memcpy` appends and an integer formatter, not `snprintf`.
- Allocate per-request data from `conn->arena`, not `malloc`. Emit JSON through yyjson with `arena_yyjson_alc`.
- No syscall on a path that changes nothing (`events_watched`; enforced on kqueue only, see Event loop).
Verification tools: `make bench`, `make test` (14 suites), `make SANITIZE=1 BUILD_DIR=build-asan test` (ASan + UBSan),
`make fuzz` (mutation fuzzer over parser/router/response), `make check-docs`. Run sanitizers and fuzz after touching
`http_parser.c`, `router.c`, `response.c` or `arena.c`. The Makefile tracks header dependencies (`-MMD`).

## Known gaps (verified, not fixed)
- **A malformed request line gets no response.** `request_framing` returns `-1` with `header_len == 0` when picohttpparser rejects the
  request (`GET /\r\n\r\n`, `HTTP/2.0`, garbage), and `request_is_complete` checks `header_len == 0` before it checks the code, so it answers
  "need more". Observed on a live server: no reply and the socket stays open until 8 KiB arrive (431) or 60 s pass (408). Well-formed requests with bad `Content-Length`,
  too many headers, or a too-long path are answered correctly (400 / 400 / 414). Likely fix: test `content_length < 0` before `header_len == 0` in `request_is_complete`.
- **Path parameter names are per tree position, not per route.** Routes `/orders/:id/items` and `/orders/:oid/notes` share one parameter node named after
  the first registration, so `req_get_param(req, "oid")` returns `NULL` (the value is under `id`). A route registered after a mid-pattern `*` at the same position
  (`/x/*/y`, then `/x/:id/z`) captures nothing. Use the same `:name` at the same position across routes.
- **Pipelining is dropped.** After the first request is answered, `flush_connection` sets `in_len = 0`, discarding any further
  request already in the buffer (client sees one response for two requests). Fix needs the parser to report bytes consumed,
  `memmove` of the remainder, and re-running the parse loop after each flush.
- **Chunked request bodies are re-scanned from the start on every `recv`** (`request_is_complete` → `chunked_body_scan`):
  quadratic in the worst case up to 10 MiB, so a slow-drip client can burn CPU. Fix: keep scan position and decoded length per connection.
- **Per-connection footprint is about 25 KB resident on macOS (72 KB allocated: 64 KiB arena + 8 KiB `in_buf`)**, up from about 7 KB before the arena; 10,000 idle connections are on the order of 250 MB there.
- **`Request.body` is still a copy** (now into the arena), including a 1-byte allocation for empty bodies.
- Path/query params over 63 chars and queries over 255 chars are truncated silently. So are individual cookie values
  over 255 chars after the `Cookie` header is split (`parse_cookies` → `cookie_values[MAX_COOKIES][256]`) - S5 fixed
  the *raw* `Cookie:` header line (`req->header_values`, now `MAX_HEADER_VALUE_LEN` before a 431), not each cookie's
  own value once split out of it, so a single very long session-token cookie among several shorter ones can still be
  truncated even though the header line as a whole fit. Request header values otherwise are not truncated: S5 raised
  the per-header cap to `MAX_HEADER_VALUE_LEN` and rejects anything still over it with 431 instead of truncating -
  see "Behavior reference, Request parsing".
- **Multiple `res_send` calls, chunked growth and static files leave dead copies in the arena** until the request ends (see Memory model).
- The io_uring backend is used only as a readiness poller; sockets are still read and written with `recv` / `write`.
- No HTTP/2, `Expect: 100-continue`, compression, `Range`, or WebSocket.
- **`res_send_file` and large (uncached) static files are still not optimized** (`improvements.md`, P1's other sub-items,
  not addressed by the static-file cache above): the response head and the file body still go out as separate `write`
  calls (no single buffer / `writev`), and large files are read with plain `read`/`write` in `STREAM_CHUNK_SIZE` pieces
  rather than `sendfile(2)`. No `ETag`/`Last-Modified`/`304`/`Cache-Control` on any response, static or otherwise.

## Where to change what
Add a response helper → `response.c/h` + `tests/test_response.c` + `API.md`. Add a parser feature → `http_parser.c/h` +
`tests/test_http_parser.c` (or `test_http_hardening.c` for a bug regression) + a case in `tests/fuzz_parser.c` seeds. Add a route feature → `router.c/h` + `tests/test_router.c`.
Add middleware behavior → `middleware.c` + `tests/test_middleware.c`. New public function → declare it in the header and list it in
`API.md` (`make check-docs` enforces this). New recipe → `examples/cookbook.c` + `tests/test_cookbook.c`. Anything allocated per request → the arena.
