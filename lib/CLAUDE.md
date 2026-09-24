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
A process runs one single-threaded, non-blocking event loop: kqueue on macOS/BSD, epoll on Linux, or io_uring on Linux
only with `CEXPRESS_EVENT_LOOP=io_uring` (readiness only: a one-shot `POLL_ADD` per fd, re-armed after every event to give
level-triggered readiness, then ordinary `recv`/`write`; needs liburing, kernel 5.13+ tested only on 6.8), chosen per process
at runtime (see "Backend selection" below); on macOS epoll is only built for `make test_epoll` through epoll-shim. `workers != 1` forks N such processes. On
Linux, each opens its own listen socket sharing the port via `SO_REUSEPORT` (4-tuple hashing balances them). On macOS/BSD
(`CEXPRESS_SINGLE_ACCEPTOR`: `SO_REUSEPORT` does not balance there, MEASURED over 90% of load on one worker of four),
only the master binds and `accept()`s; each accepted fd is handed to a worker over a private socketpair via `SCM_RIGHTS`,
round-robin (see "Behavior reference, Workers and fork"). Either way, a master respawns any worker that dies, with backoff
and a restart budget (same section). Handlers run synchronously on the loop: a blocking call (DB,
sleep) stalls that whole worker, so scale with workers, not threads. State is per process; there is no shared memory.
If io_uring is requested but refused (Docker's default seccomp profile does this: `EPERM`), `event_loop_init` logs why and fails, and `app_listen_worker` exits; it never falls back to epoll.

Per request (`connection.c: handle_readable` → `serve_buffered_requests`, which repeats steps 1-6 for every complete request already in `in_buf`, starting at `conn->in_off` - pipelining, see "Behavior reference, Pipelining"):
1. `recv` into `conn->in_buf` - the worker's shared `App.read_buf` when nothing is buffered for this connection (see Memory model) - then one `parse_request_head_resume` call (`http_parser.c`: runs picohttpparser once over the headers; while no end-of-head blank line has arrived it only extends the blank-line search from `conn->head_scan`, so a head that trickles in one byte per `recv` is searched once and parsed once instead of re-parsed from byte 0 on every `recv`) fills a `ParsedHead` reused by every check below - the body-limit check, `request_head_is_complete` (a chunked scan when the body is chunked, resumed from `conn->chunk_scan` so each body byte is scanned once per request, not once per `recv`), and the full parse, instead of each running its own independent pass over the same bytes (up to four per request previously).
2. `parse_http_request_in_place(in_buf + in_off, avail, &head, &req, conn->arena, &body_saved)` → `Request` on the stack (copies method/path/query into its fixed arrays; headers and the body are VIEWS into `in_buf`, not copies; a chunked body is decoded in place over its own framing). The body's NUL terminator overwrites the byte after it (for `Content-Length`, the first byte of a pipelined next request); `serve_buffered_requests` restores it (`body_saved`) right after `dispatch`, before `flush_connection`. Failure → reject (400 / 413 / 414 / 431), close. (`parse_http_request_from_head` - the copying variant - and `parse_http_request`/`request_is_complete`/`request_framing` remain as unchanged-behavior functions over `parse_request_head` for callers - tests, `fuzz_parser.c` - that only need one piece.)
3. `keep_alive = !request_wants_close && !shutting_down`; `res.is_head_request` set.
4. `match_route` (per-method Patricia tree; the walk only finds the `Route`, then `:params` are filled from that Route's own pattern via `match_path` - so routes may name the same tree position differently; skipped when `Route.has_params` is 0) → `dispatch` (`middleware.c`): app-wide middleware
   (prefix-filtered) → route middleware → handler, or the default 404 / 405 / OPTIONS answer, or a static file.
5. The handler calls `res_*` (`response.c`), which only builds bytes into `conn->out_buf` (allocated from the arena). `res_stream` builds only the head there and records a producer on the `Connection`; the body is produced later, by `flush_connection` (see "Behavior reference, Producer streaming").
6. `flush_connection` writes. Keep-alive: `arena_reset`, advance `in_off` past this request's `request_len` (or `in_len = 0` when nothing follows it), zero `chunk_scan` and `head_scan`, free `in_buf` (or hand back the borrowed `App.read_buf`) when nothing is buffered. Otherwise `connection_close`, dropping anything pipelined behind it.

Only `connection.c`, `event_loop_*.c`, `cluster.c` do I/O. Parsing, routing, dispatch and
response building never touch a socket, so tests drive them with a fake `Connection` whose arena is a static buffer
(see `tests/test_cookbook.c: fetch`). Keep it that way.

## Files
| File | Responsibility |
|---|---|
| `app_types.h` | every struct/typedef and every compile-time limit |
| `arena.c/h` | bump allocator (`arena_alloc`, `arena_reset`, `arena_yyjson_alc`); one shared per worker process (`App.arena`), not one per connection |
| `http_parser.c/h` | request parsing on top of picohttpparser, framing (Content-Length / chunked), accessors, `status_text`. One `parse_request_head` pass feeds the body-limit check, completeness check and full parse; `request_framing`/`request_is_complete`/`parse_http_request` are thin wrappers kept for existing callers. Headers are stored as views into the input buffer and cookies are split lazily, on first access |
| `router.c/h` | route registration, one Patricia (segment-radix) tree per method, `app_mount`, `app_serve_static`, `app_free_routes` |
| `middleware.c/h` | pipeline (`chain_next`, `chain_error`, `dispatch`), 404/405/OPTIONS defaults |
| `response.c/h` | response head assembly, cookies, chunked streaming (`res_write`, buffered), producer streaming (`res_stream`: builds the head and records the producer; `stream_write` frames chunks), file streaming |
| `connection.c/h` | accept, read/parse/dispatch/flush, buffer growth, idle timeout, shutdown, listen |
| `event_loop.h` + `event_loop_kqueue.c` / `event_loop_io_uring.c` / `event_loop_epoll.c` | one API over three backends (fds, timers, signals). On Linux `event_loop_linux.c` implements `event_loop.h` by forwarding through `App.loop_ops` to the static functions behind `io_uring_loop_ops` / `epoll_loop_ops` (`event_loop_backend.h`, private); kqueue implements it directly |
| `cluster.c/h` | fork workers, respawn (with backoff and a restart budget), drain; on macOS/BSD (`CEXPRESS_SINGLE_ACCEPTOR`) also the single acceptor - binds the one listen socket, `accept()`s, and hands fds to workers round-robin over per-worker socketpairs |
| `static.c/h` | traversal-safe file serving, with an in-memory cache of recently served files |
| `multipart.c/h`, `urlencoded.c/h` | form body parsers (handler-invoked, not automatic). Multipart part headers are found line-anchored and their parameters parsed as a `;`-list of token / quoted-string values, never substring-searched; `MultipartPart.filename` stays the raw client string, `multipart_safe_filename` is the filesystem-safe basename |
| `vendor/picohttpparser/` | vendored HTTP/1.x request parser (MIT/Perl) |
| `vendor/yyjson/` | vendored yyjson 0.13.0; JSON reading and writing. `cexpress.h` includes it. There is no engine JSON layer of its own |
| `examples/cookbook.c` | tested few-shot recipes; `tests/test_cookbook.c` runs every one |

## Limits (all compile-time, in `app_types.h`; excess is truncated or dropped, never overflowed, except where marked)
Routes: no fixed cap per App (each is malloc'd into a tree), 64 per Router (`MAX_ROUTER_ROUTES`), 16 distinct methods · app middleware 16 · route middleware 8 ·
path params 8 (value 63) · query params 16 (63) · request headers 32 (`MAX_HEADERS`; **a 33rd header is a 400**; a
header name/value has no length cap of its own - it is a view into the input buffer, not a fixed-size copy -
only the whole header block fitting `BUF_SIZE` bounds it) ·
cookies 16 (255) · request headers total 8 KiB (`BUF_SIZE`, else 431) · path 255 (else 414) · query 255 ·
body 10 MiB (`MAX_BODY_SIZE`, else 413) · response headers 16 (name 63, value uncapped: copied into the arena) · Set-Cookie 16 (512 each) · trailers 8 (same as headers) · multipart parts 16 ·
form fields 32 · static file 50 MiB · static file cache 256 entries, 256 KiB each, 64 MiB total, 1 s revalidation
(`STATIC_CACHE_*`, `static.c`; a file over the per-entry cap is served but never cached; see "Static" below) ·
producer stream output 16 KiB per producer call (`STREAM_CHUNK_SIZE`; one `stream_write` ≤ `STREAM_WRITE_MAX`) and no total cap · idle timeout 60 s · request header deadline 10 s · request body deadline 30 s ·
pending-response write-stall deadline 30 s · drain deadline 5 s · response head 8 KiB (larger: the connection is closed without a response, logged; the only bound on a response header value) ·
worker init hooks 4 · cluster workers 128 · arena 64 KiB, one per worker process, not per connection (see below; exceeding it falls back to malloc, it is not a limit) ·
max connections 10,000 per worker (`ServerConfig.max_connections`, `DEFAULT_MAX_CONNECTIONS`; a runtime config field, not a compile-time-only limit like the others here - `0` opts out, uncapped) ·
body limit prefixes 16 (`MAX_BODY_LIMITS`; `App.body_limits`, set at runtime by `app_use_body_limit`, unlike the other limits here - see "Body limits" below).

## Memory model
**One arena per worker process, not per connection.** `app_init` mallocs a single 64 KiB buffer and calls
`arena_init` once into `App.arena`; `connection_create` just points `Connection.arena` at it
(`conn->arena = &app->arena`) rather than allocating one of its own. Every accepted connection is now just
`calloc(sizeof(Connection))` - no input buffer either (next paragraph). This is safe under the single-threaded,
non-blocking event loop model: at most one connection's handler code runs at a time, and `handle_readable`/
`reject_request` reset the shared arena (`arena_reset(&app->arena)`, through `app`, not `conn` - `conn` may already
be freed by then) exactly once, right after each dispatch-and-flush cycle they run - by which point
`flush_connection` has already copied any still-unsent response tail out to a connection-owned buffer if it
couldn't fully drain in that same cycle (see `Connection.out_buf_owned`), so nothing any connection still needs
is ever left in the shared arena when another connection's turn begins. File streaming (`res_send_file`) never
touches the shared arena at all: each chunk is read into `Connection.stream_buf` (shared with `res_stream` producers), a connection-owned buffer
malloc'd lazily on first use and reused turn to turn, precisely because a large file spans many event-loop turns
during which other connections' dispatches would otherwise reuse and overwrite an arena-resident chunk buffer.
Measured on macOS, 5,000 idle keep-alive connections on one worker now take about 8.2 KB RSS per connection (about
44 MB total for 5,000), down from about 25 KB per connection (about 121 MB) before this fix (Linux not measured);
the shared receive buffer below removed the remaining 8 KiB `in_buf`.
**One receive buffer per worker process, borrowed per read.** `app_init` also mallocs `App.read_buf`
(`BUF_SIZE`, 8 KiB). A connection with nothing buffered has `in_buf == NULL`; `handle_readable` (`read_and_serve`)
points `in_buf` at `App.read_buf` for the `recv`, and complete requests are parsed and served in place there (header
views point into it; they are dead once the handler returns). Before `handle_readable` returns with the connection
still open, `stop_borrowing_read_buf` either hands the buffer back (nothing left) or copies the unserved bytes
`[in_off, in_len)` - a partial request, or requests pipelined behind a pending response - into a connection-owned
`BUF_SIZE` malloc, compacted to offset 0 (`request_len`, `chunk_scan` and `head_scan` are relative to `in_off`, so they stay
valid). Invariant: **`in_buf` never points at `App.read_buf` between event-loop turns**, so a single shared buffer is
enough. `grow_in_buf` never reallocs the borrowed buffer (it mallocs the grown size and copies);
`connection_close` never frees it (`app_destroy` does). `flush_connection`'s keep-alive reset frees the owned buffer
(however far it grew) once `in_len == 0`. The only added cost is one copy of a partial request's bytes, per partial
read-turn that leaves it incomplete; complete requests - the common case - are never copied.
Measured on macOS (demo, one worker, 5,000 connections, RSS delta): about **239 B per idle keep-alive connection**
after one request and **216 B** for an accepted-but-silent one, down from about 8.4 KB each before the shared receive buffer (44 MB → 1.2 MB
for 5,000). Kernel socket buffers are not in RSS. `/ping` throughput unchanged (wrk, 50 connections, three rounds).
The arena serves everything that lives for one request (except `Request.body`, a view into `in_buf` - see
"Behavior reference, Request parsing"): the initial `conn->out_buf` build (`res_*`), response header and trailer
values (`ResponseHeader.value`, copied by `res_set_header`/`res_set_trailer`; an overwrite leaves the old copy until reset),
chunked-response growth, and any yyjson document created with `arena_yyjson_alc`. Bump allocation, 8-byte aligned,
no per-allocation free. When the remaining space is too small (not only for a single request over 64 KiB), the
allocation falls back to `malloc` and is chained in a list that `arena_reset` frees. Consequences: nothing reached
through `req` or `res` may be kept past the handler; a growing chunked response copies into a new arena block each
doubling and leaves the old block in the arena until the request ends; a static file is read into a malloc'd buffer
and copied again into the arena by `res_send_bytes` (unlike `res_send_file`, which never copies the body into the
arena at all - see above).

## Ownership (who frees what)
| Thing | Allocated by | Freed by |
|---|---|---|
| `App.arena`'s 64 KiB buffer (one per worker process, not one per connection) | `app_init` (one malloc) | `app_destroy` (`arena_destroy` for fallback blocks, then a plain `free` of the buffer itself - `arena_destroy` never frees `buf`, same convention as a test's hand-built Arena) |
| `Connection` | `connection_create` (one calloc; no arena buffer behind it any more) | `connection_close` (exactly once) |
| `conn->in_buf` (NULL while nothing is buffered) | not allocated while it borrows `App.read_buf` (during one `handle_readable`); owned copy: `stop_borrowing_read_buf` (unserved bytes left at the end of `handle_readable`) or `grow_in_buf` (a body past `BUF_SIZE`; realloc on further growth) | the owned copy: `flush_connection`'s keep-alive reset once nothing is buffered, or `connection_close`. The borrowed `App.read_buf`: never through `conn` |
| `App.read_buf` (`BUF_SIZE`, one per worker process) | `app_init` | `app_destroy` |
| `conn->arena` | not allocated - always `&app->arena`, set once at `connection_create` | nobody frees it through `conn`; `app_destroy` frees the one underlying `App.arena` after every connection is already closed |
| `conn->stream_buf` (a connection-owned `STREAM_CHUNK_SIZE` turn buffer, lazily malloc'd, reused chunk to chunk - never the shared arena) | `flush_connection`, on the first chunk of a `res_send_file` response or the first producer call of a `res_stream` response | `flush_connection` when streaming ends (success or a mid-stream error) or `connection_close` (a still-streaming connection closed some other way) |
| `conn->stream_ctx` (the application's producer state, handed over by `res_stream`) | the handler (application code), before `res_stream` | `stream_release` (`response.c`) calls `stream_ctx_free(ctx)` exactly once: after `STREAM_END` (in `flush_connection`), on `STREAM_ABORT` or any close (`connection_close`), when a later `res_*` in the same handler replaces the stream, or inside `res_stream` for HEAD. If `res_stream` returns -1 the caller still owns it |
| Arena fallback blocks | `arena_alloc` when the buffer is full | `arena_reset` (each dispatch-and-flush cycle, in `handle_readable`/`reject_request`) or `arena_destroy` (`app_destroy`) |
| `Request.body` | engine path (`parse_http_request_in_place`): not allocated - a view into `conn->in_buf`; `parse_http_request`/`_from_head` (tests, tools): copied into the arena. Always non-NULL after success | nobody: the input buffer's own lifecycle or the arena. Handlers never free it or keep it |
| `req_get_*` results, `MultipartPart.data` | point inside the Request / body | nobody; valid until the handler returns |
| `conn->out_buf` | `res_*`, from the shared arena (one allocation per response; a second send just leaves the first in the arena) - **or** a connection-owned `malloc`'d copy of an unsent tail (`conn->out_buf_owned`, made by `flush_connection` when a response can't be fully written in one call, since the shared arena would otherwise be reused by another connection before the write finishes) | the arena copy: nobody, reclaimed by the next `arena_reset`. The owned copy: `flush_connection` once fully drained, or `connection_close` on any error/close path - never both (see `Connection.out_buf_owned`) |
| yyjson doc built or read with `arena_yyjson_alc(res->conn->arena)` (a pointer already - no `&`) | arena | nothing: `yyjson_*_doc_free` is a no-op for it, the arena reclaims it |
| yyjson doc with a NULL allocator (e.g. `error_handler_json`) | libc malloc | `yyjson_mut_doc_free` / `yyjson_doc_free` |
| `yyjson_mut_write(doc, 0, &len)` result | libc malloc, **whatever allocator the doc uses** | caller, C `free` (forgetting it leaks once per request) |
| `Route`, `PatriciaNode`; a static mount's `Route.static_root` string (heap, `NULL` on ordinary routes and on every `Router` slot) | `app_add_route_mw`, `app_serve_static`, `tree_insert` | `app_free_routes`, called by `app_destroy`; every heap `Route` is dropped through `router.c`'s `route_free` (root + route), including registration failures and duplicates |
| `app->connections` | `app_init` | `app_destroy` |
| `App.loop_ops` (the selected Linux backend's static `EventLoopOps` table; decides which member of the `kq`/`epoll_fd`/`ring` union is live) | not allocated: `event_loop_init` points it at `io_uring_loop_ops` or `epoll_loop_ops` on success | nothing to free; `event_loop_close` sets it back to `NULL` after the backend's own close |
| `App.poll_regs` (io_uring backend only: one `PollRegistration` per fd) | `event_loop_io_uring.c`'s `registration_for`, grown by doubling on the first interest change for an fd past its size | `event_loop_close` |
| `app->spare_fd` (one `/dev/null` fd held in reserve for `EMFILE`) | `app_init` | `app_destroy`; also closed-then-reopened across its life by `accept_connections` (on `EMFILE`) and `connection_close` (opportunistic re-arm) - see Behavior reference, Overload |
| `ServerConfig.bind_address` | the application (`app.config`; `app_init` sets `NULL`) | never by the engine: borrowed, read by `create_server_socket` at bind time, so it must outlive `app_listen` |
| `create_server_socket`'s `addrinfo` list | `getaddrinfo` (libc) | `freeaddrinfo`, inside `create_server_socket` on every path past the call (right after `bind`, or on a failed `socket`/`setsockopt`) |
| `cluster.c`'s `listen_fd` (`CEXPRESS_SINGLE_ACCEPTOR` only - the master's one real listen socket, replacing per-worker binds) | `cluster_listen` (`create_server_socket`) | `cluster_listen`, after every worker has drained, at the end of the same function |
| The master's copy of each accepted client fd (`CEXPRESS_SINGLE_ACCEPTOR` only) | `cluster_listen`'s accept loop (`accept_client`) | the same loop, right after `dispatch_client_fd`, success or not - the worker owns its own `SCM_RIGHTS` copy from then on |
| `ClusterWorkerSlot.control_fd` per slot (the master-side end of that worker's socketpair; the worker keeps the other end, `sv[1]`, as its own `server_fd`) | `spawn_worker`, fresh on every spawn *and* every respawn | `spawn_worker`'s next respawn for that slot (closes the stale one first), or `cluster_listen`'s final cleanup once every worker has drained |
| Static file cache entries (cached path string + file bytes, `static.c`'s own process-lifetime global, not tied to any `App`) | `static_serve_file`, on a cache miss or a changed file | replaced in place on the next change, evicted (stalest first) once `STATIC_CACHE_MAX_ENTRIES` is reached, or all of them via `static_cache_clear` (tests; nothing in the engine calls it) |

## Return conventions
`0` ok / `-1` error for setup functions (`res_send_file`, `event_loop_*`, `create_*`).
`parse_http_request` / `parse_http_request_from_head` / `parse_http_request_in_place`: `0` ok (only then does `_in_place` write anything into `raw`), `-1` malformed (including an HTTP/1.1 request without exactly one `Host` header, a bare `\n` line ending anywhere in the request line or header block, and a `Transfer-Encoding` this engine can't frame - see `req.content_length` below), `-2` path too long (→ 414), `-3` retired (used to mean "a header name/value too long to store", impossible now that headers are views, not fixed-size copies - never returned, kept reserved rather than reused; this is the function's own top-level code, unrelated to `req.content_length`'s own `-3` below), `-4` a percent-decoded path/query name/query value contains an embedded NUL, or the path is not canonicalizable (`%2F`, a `.`/`..` segment, not origin-form; → 400); after `-1`, `req.content_length == -2` means body too large (→ 413), `req.content_length == -3` means `Transfer-Encoding` names anything other than exactly the single token `chunked` (→ 501).
`url_decode` / `parse_query_string`: `0` ok, `-1` a decoded byte was NUL - the destination is still fully written and NUL-terminated, but the caller must treat it as invalid input rather than use it.
`url_decode_span`: `0` ok, `-1` a decoded NUL, `-2` the decoded bytes did not fit `dst` (truncated prefix left in `dst`). The internal `decode_bounded` returns the same three codes; path decoding sizes `dst` so `-2` cannot occur there, and `parse_query_string` treats only `-1` as an error (query values are still truncated to their slot).
`parse_urlencoded_body`: field count (`>= 0`), `-1` a decoded name/value holds a NUL, `-2` a decoded name > 63 or value > 255 bytes; on either error `field_count` is 0 so no field is usable (the handler answers 400). Fields are decoded directly from `body` into the slots (no pre-decode copy), so the slot limit is on the decoded length.
`request_is_complete` / `request_head_is_complete`: `1` for a complete request, for invalid `Content-Length` / chunked+`Content-Length` framing, and for a request line or header block picohttpparser rejects, once the head's blank line has arrived (stop reading in every one of these cases, let the parser report the specific error - malformed used to be indistinguishable from "need more bytes", since both leave `header_len == 0`; told apart via `ParsedHead.content_length`, `-1` only on the malformed path). `0` only while more bytes are genuinely needed: before any blank line (even when what has arrived is already malformed: picohttpparser runs only once a blank line is in, after the first look at a request - that keeps the per-`recv` cost of an unfinished head linear, and such a head is answered 400 at its blank line, or 431 at `BUF_SIZE` / 408 at the header deadline), or while a well-framed head waits on a short body. A head picohttpparser still calls incomplete past a blank line is malformed (its `parse_http_version` wants 9 bytes before reading any, so `GET / X\r\n\r\n` used to wait for the request header deadline). Once `1` for a prefix, `1` for every longer buffer, with the same answer, so the verdict does not depend on where `recv()` split the bytes (asserted per input by `fuzz_parser.c` and end to end by `test_answered.c`). `chunked_body_scan` / `chunked_body_scan_resume`: `1` done, `0` need more, `-1` malformed, `-2` too large. The resume variant's `ChunkScanState` (offsets from the body start, so `in_buf` reallocs don't invalidate it) only ever advances past fully received and validated chunks, and the trailer's `\r\n\r\n` search resumes 3 bytes before where it last gave up; a resumed verdict is therefore identical to a from-scratch scan of the same bytes (asserted per-prefix in `test_http_parser.c` and at random split points in `fuzz_parser.c`). `parse_request_head_resume`'s `head_scan` (offset from the request start) is where the blank-line search restarts: 2 bytes before the end of the last buffer searched, since a blank line (`\n\n` or `\n\r\n`) that began there was not visible yet; `0` means "first look", when picohttpparser runs straight away (a complete head proves its blank line, so a request that arrives whole is still one pass). Its result equals `parse_request_head`'s for every buffer (asserted per prefix in `test_http_hardening.c`). State is per request: `Connection.head_scan` and `Connection.chunk_scan` are zeroed by `connection_create`'s `calloc` and by `flush_connection`'s keep-alive reset. `parse_http_request_from_head` still runs one from-scratch scan plus decode on completion (linear, once).
`parse_request_head`: same codes as `request_framing` (`0` absent/zero-length or incomplete, `>0` value, `-1` malformed/conflicting, `-2` oversized) - check `ParsedHead.header_len == 0` to tell "incomplete" apart from "malformed" at this layer (both still return via that ambiguity; `request_head_is_complete`, above, is what resolves it before handing off to the rest of the pipeline).
yyjson: read functions return `NULL` on failure; `yyjson_mut_*_add_*` return `false` on failure (the cookbook and demo do not check them).
Accessors return `NULL` for "absent". Nothing in the engine uses exceptions or `errno` for logic outside the socket layer.

## Behavior reference (non-obvious rules; the code is the spec for the rest)
- **Routing.** Deny by default. Exact method match; no path match → 404; path matches another method → 405 + `Allow`.
  Each method has its own Patricia tree keyed by path segments (split on `/`, empty segments ignored, so `/a//b/` = `/a/b`). At each node the
  search tries, in this order and with backtracking: a literal child, then the `:name` / mid-pattern `*` child, then a trailing `*`.
  So **specificity beats registration order**: `/users/me` wins over `/users/:id` even when registered second.
  A mid-pattern `*` matches one segment and captures nothing. A trailing `*` matches one or more segments (never the bare prefix). A duplicate pattern for the same method keeps the first and warns.
  **Registration failure is fail-soft, never a truncated/corrupted route.** A method/path that doesn't fit
  `Route.method`/`Route.path` (7/255 chars) is rejected outright by `fill_route` with a `stderr` message and the
  route is not registered at all - it used to be silently `strncpy`-truncated into a shorter pattern than the
  caller asked for, so a request could match a route nobody actually meant to register. Every allocation on the
  registration path (`app_add_route_mw`'s/`app_serve_static`'s `Route` `malloc`, `create_patricia_node`'s
  `calloc`/`malloc`, `tree_insert`'s `children` `realloc`) is checked the same way: a failure logs to `stderr`,
  frees the `Route` being inserted, and registers nothing, rather than dereferencing a partially-built node or
  corrupting `PatriciaNode.children`. Tree structure already linked in before a failing allocation is left in
  place, not unwound - a `PatriciaNode` with `route == NULL` is already a normal internal node, shared by any
  other route under the same path prefix.
  **Static children are sorted, not registration order.** A node's `children` array is kept sorted by segment
  (short-lexicographic: shared-prefix bytes compare first, the shorter segment sorts before a longer one that starts
  with it) and searched with binary search (`find_child`) instead of a linear `memcmp` scan, both on `tree_insert`
  (new child inserted at its sorted position via `memmove`) and on `tree_search_recursive`'s per-segment lookup - a
  path segment with many static siblings (`/api/<many resources>`) is now O(log siblings) instead of O(siblings).
  MEASURED: 8,401 ns → ~80 ns per lookup at 5,000 siblings on a scratch benchmark (one worker
  process, 1,000,000 lookups). Only static children are affected; `param_child`/`catch_all_child` are still single
  pointers, unaffected. Irrelevant for a hand-written route table (dozens of siblings at most); matters for a
  generated one.
  `HEAD` falls back to the same path's `GET` route (body suppressed, `Content-Length` kept). `OPTIONS` on a known path
  → 200 + `Allow` (with `HEAD` added if `GET` exists). Explicit `app_head` / `app_options` win. Path is percent-decoded and then canonicalized before routing (see "Request parsing, Canonical path"), so routing, prefix middleware, body limits and static mounts all see the same segments.
  `match_path` is a standalone pattern matcher that the router itself no longer calls; it is kept for tests and does not reproduce every tree rule (it is first-match, per pattern).
- **Middleware.** Order = registration order (app-wide, only entries whose prefix matches at a segment boundary - `path_prefix_matches`, the same helper `app_body_limit_for_path` uses; prefixes are stored normalized by `path_normalize_prefix` at registration, so `"/admin/"` or `"admin"` scope exactly like `"/admin"`; runs for 404s
  too) → route middleware → handler. One error handler (`app_use_error`, last wins); default is `res_status` + `res_send`.
  Handlers get no chain and cannot call `chain_next` / `chain_error`. Code after `chain_next` sees the final `res->status`.
- **Sub-routers.** `app_mount` copies routes (prefix prepended; `/` mounts at the bare prefix) and turns `router_use`
  middleware into prefix-scoped app middleware. The Router may be a stack local. No nesting.
- **Static.** `app_serve_static` registers `GET <prefix>/*`, `realpath`s the root once (a missing root registers nothing and logs it, so every request under the prefix is a 404), refuses `..` (403), answers 404 for any request segment starting with `.` (dotfiles like `.env`, `.git/config`; a dot-directory in the root path itself is fine), sends `X-Content-Type-Options: nosniff` on every answer, re-checks the
  resolved path stays under the root after symlink resolution (403), 404 for non-files, serves `index.html` for a directory,
  never lists. A file up to `STATIC_CACHE_MAX_ENTRY_BYTES` (256 KiB) is read whole and sent with `res_send_bytes` (and cached, below); a
  larger one (up to `MAX_STATIC_FILE_SIZE`, 50 MiB, else 500) goes through `res_send_file`, which streams it from an open fd in
  `STREAM_CHUNK_SIZE` pieces through `conn->stream_buf` - never read whole, never in the arena, so a slow reader costs 16 KiB,
  not the file size, and the event loop never blocks on one big `fread`. MEASURED with a 40 MiB file: RSS 1.5 MB after one
  download and 1.7 MB during ten 20 KiB/s downloads (was 83 MB and 452 MB). The root is resolved against the process's working directory.
  **File cache.** `static_serve_file` caches files up to `STATIC_CACHE_MAX_ENTRY_BYTES` after their first read, keyed by
  the pre-`realpath` candidate path (`static_root` + the already-traversal-checked subpath), not the resolved one - MEASURED
  a 6.6x gap between the static-mount path and an equivalent in-memory response, almost entirely
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
- **Request parsing.** picohttpparser does the request line and header block; it is strict about tokens and
  rejects HTTP versions other than 1.x. `Content-Length`
  must be plain digits; duplicates must agree; `Content-Length` together with chunked is 400 (smuggling shape). Header names are matched exactly and case-insensitively (never by substring). Method ≤ 7
  chars. Query and cookies are still parsed eagerly into fixed arrays (cookies lazily *triggered*, see below, but the
  arrays themselves are fixed-size once triggered); headers are not copied at all. More than 32 headers → 400 (checked
  explicitly against `MAX_HEADERS` in `parse_http_request_from_head`, since the single-pass head parse raised the underlying `phr_parse_request`
  capacity itself to `MAX_FRAMING_HEADERS`, above `MAX_HEADERS`, precisely so this case is diagnosed as malformed rather
  than mis-reported as "incomplete" - see "Hot-path rules"). A request line or header block picohttpparser rejects
  outright is likewise 400, immediately (fixed 2026-09-23 - see "Return conventions" above and `improvements_progress.md`),
  not left open waiting for headers that will never arrive.
  **Framing ambiguity (fixed 2026-09-23).** picohttpparser itself tolerates a bare `\n` as a line terminator
  anywhere one is expected (the request line, any header line, the final blank line) - a leniency a front proxy
  reading strictly per RFC 9112 (CRLF only) would not extend, so the two could disagree about where one request
  ends and the next begins. `parse_request_head` now scans the located header block for any `\n` not immediately
  preceded by `\r` and rejects the whole request (400) if found, rather than accepting whatever picohttpparser
  happened to tolerate. Separately, `Transfer-Encoding` used to be detected with a bare substring search for
  "chunked" anywhere in the header value, so `Transfer-Encoding: xchunked` counted as chunked framing and
  `Transfer-Encoding: gzip, chunked` was accepted and treated as plain chunked with the `gzip` half silently
  ignored. Both are now matched token-exact (`is_sole_token`, `http_parser.c`): a `Transfer-Encoding` value is
  accepted as chunked framing only when it reduces, after splitting on commas and trimming OWS, to exactly one
  token that case-insensitively equals `chunked` - this engine implements no other transfer-coding. Anything else
  (a substring lookalike, `chunked` alongside any other coding regardless of order, an unsupported coding alone,
  or the coding split across separate duplicate `Transfer-Encoding` header instances) is rejected with 501 `Not
  Implemented`, not silently accepted or ignored.
  **Header storage.** `req->headers` holds `struct phr_header` VIEWS (`name`/`value` point into `in_buf`, not
  NUL-terminated) instead of copies into fixed-size arrays, so there is no per-header length cap left to enforce - a
  header of any length that fits within the whole header block (`BUF_SIZE`, 8 KiB, unrelated to this) is accepted.
  This retired the old `-3`/431 return code (`parse_http_request` never returns `-3` any more; kept reserved, not reused,
  so old code branching on it is merely dead), superseding the interim fix of simply raising the old fixed-size cap. `req_get_header` materializes a
  NUL-terminated copy of the matching view into `req->arena` (set by every `parse_http_request*` to the arena it was
  given) on every call - not cached, since a handler reads a given header only a handful of times per
  request at most. `parse_headers`, the standalone component parser `tests/` call directly (not on the live request
  path), was changed the same way and takes an `Arena *` now for the same reason - it no longer truncates either.
  **Cookie splitting is lazy.** `parse_http_request_from_head` no longer calls `parse_cookies` itself; `req_get_cookie`
  does, once, the first time it is called for a given request (`req->cookies_parsed`) - a request that carries a
  `Cookie` header but whose handler never reads one never pays for the split. Cookie storage itself (`cookie_names`/
  `cookie_values`, fixed 64/255-char slots) is unchanged; only *when* the split runs moved.
  **Host (fixed 2026-09-24).** An HTTP/1.1 request must carry exactly one `Host` header (name matched exactly,
  case-insensitively; an empty value is allowed), else `parse_request_fields` returns -1 → 400 and close (RFC 9112
  §3.2: with two, a proxy and the app can act on different hosts). HTTP/1.0 is exempt. The count is taken in the
  same loop that stores the header views, so it adds no extra pass.
  Chunked bodies: a chunk-size line is exactly 1–16 hex digits, then nothing or BWS `;` extension (no `0x`, sign, leading or
  trailing whitespace - a proxy that reads `0x5` as 0 would frame the body differently); extensions ignored but free of control
  characters; trailer lines held to the header block's rules (CRLF only, no bare CR/LF, no control character but HTAB), then discarded; decoded size capped at `MAX_BODY_SIZE`, raw wire size capped at `header_len + MAX_BODY_SIZE`.
  **Body is not copied.** On the engine path `req->body` points at `in_buf + header_len`. A chunked body is decoded
  in place by `chunked_body_decode(body_start, avail, body_start)` (it uses `memmove`: decoded output never overtakes the
  framing being read, since each chunk moves left by at least its own size line). `body[content_length]` is set to
  `'\0'`; that byte is the one after a `Content-Length` body - `in_buf[in_len]` (always writable, already `'\0'`) or a
  pipelined next request's first byte, which `serve_buffered_requests` restores after `dispatch`. For chunked the NUL
  lands inside the request's own consumed framing, so nothing outside the request changes. The request's wire bytes are
  altered by a chunked decode, which is safe because `request_len` and every header view were computed first and the
  bytes are consumed once the response is queued. `res_*` copy everything they send, so nothing points into `in_buf`
  after `dispatch`. MEASURED: peak RSS for a 9.5 MiB upload 41.7 → 32.1 MB (Content-Length and chunked alike); what
  remains is mostly the transient old+new block during `grow_in_buf`'s doubling `realloc`.
  **Canonical path (fixed 2026-09-23).** The router skips empty segments, but prefix middleware compared
  `req->path` byte for byte, so `//admin/secret` (and `/%2Fadmin/secret`, since `%2F` decoded into a `/`) reached
  `/admin/secret` without running `app_use_prefix(app, "/admin", auth)` - MEASURED auth bypass. `parse_request_fields`
  now refuses (400, `-4`) a raw `%2F`/`%2f` before decoding, then `path_canonicalize` (`http_parser.c`, pure) collapses
  repeated `/` in the decoded path (at most one trailing `/` kept) and refuses (400) any `.`/`..` segment - including
  one produced by `%2e` - and any target not starting with `/` (absolute-form, or a bare `admin/x` that the router
  would still have matched), except exactly `*`. Dot segments are refused rather than resolved: clients already
  remove them (RFC 3986) and the static mount's own `..` check stays as defense in depth. Invariant: after a
  successful parse `req->path` is canonical, and every prefix consumer (middleware, body limits, mounts) matches it
  with `path_prefix_matches` against a prefix normalized by `path_normalize_prefix`. The target-to-path step
  (query dropped, `%2F` refused, decoded, canonicalized) is one pure function, `request_target_path`, used both by
  `parse_request_fields` and by the body-limit check, which runs before a `Request` exists.
  **Embedded NUL.** The path and query names/values are percent-decoded (`decode_bounded`/`url_decode`); a decoded byte
  that is NUL (`%00`, or a raw NUL byte already in the request line) is rejected with 400 (`parse_http_request`'s `-4`)
  rather than silently truncating everything downstream that reads `req->path`/`req_get_query` as a C string - MEASURED
  `GET /static/style.css%00.png` used to be routed and served as `/static/style.css`, a bypass for
  any suffix/extension check performed on the path before use. Header and cookie values are not percent-decoded by this
  engine at all, so this vector does not apply to them (`req_get_header`/`req_get_cookie` already return raw bytes;
  header values have no length limit at all, see above; a cookie's own value, once split, still has one).
- **Buffers.** Input is read into the shared `App.read_buf` (8 KiB) and only copied to a connection-owned 8 KiB
  `in_buf` when bytes are left unserved (see Memory model); with headers complete and a body pending it grows by doubling, capped at the
  known target size (`Content-Length` and chunked both work this way now - `Content-Length` used to realloc straight
  to `header_len + content_length + 1` in one step, reserving virtual memory proportional to what the client merely
  *declared* rather than what it had actually sent), and freed once nothing is buffered. No header
  terminator within 8 KiB → 431.
- **Pipelining.** Several requests may sit in `in_buf` at once. `serve_buffered_requests` answers them strictly in
  order, one `flush_connection` each, starting at `Connection.in_off`; `request_wire_len` (`http_parser.c`: `header_len` +
  `Content-Length`, or + `ChunkScanState.body_end` for chunked, which ends after the trailer's blank line) records where
  the next one starts (`Connection.request_len`), and `flush_connection`'s keep-alive reset advances `in_off` by it.
  Bytes after a `Content-Length` body are the next request, never part of this body. The unserved tail is moved to the
  front of `in_buf` only when more input must be read (`compact_in_buf`, on "need more"), never after each request, so
  each byte moves at most once (per-request `memmove` would be quadratic for a large batch of tiny requests).
  The shared receive buffer adds one more move: when `handle_readable` returns with requests still unserved in the borrowed `App.read_buf`
  (cap hit, or a response pending), `stop_borrowing_read_buf` copies `[in_off, in_len)` into an owned buffer at offset
  0, so `in_off` is 0 again once `handle_readable` returns; it only goes above 0 inside an owned buffer while
  `handle_writable` serves from it.
  **Fairness:** at most `MAX_PIPELINED_PER_EVENT` (16) requests per readiness event; past that the connection arms write
  interest and the rest is served by `handle_writable` on the next poll (a connected socket is almost always writable).
  **Backpressure:** while a response is pending (`EAGAIN`), `flush_connection` (`wait_for_writable`) drops read interest
  and `handle_readable` consumes nothing, so a request pipelined behind a large response is never dispatched over the
  pending one (previously it was: the buffer still held the first request, which was re-parsed and dispatched again,
  overwriting and leaking the owned tail copy). Read interest returns in the keep-alive reset. A client that pipelines
  without reading is therefore stopped by TCP flow control, not buffered by the server.
  **Syscalls:** `handle_readable` returns as soon as a `recv` produced at least one answered request instead of calling
  `recv` again for an almost-certain `EAGAIN` (level-triggered readiness re-fires if more is waiting) - MEASURED: the
  extra `recv` cost ~9% of non-pipelined `/ping` throughput. A rejected request (400/413/414/431/501) or a response
  with `Connection: close` (including every response during shutdown) closes the connection; whatever was pipelined
  behind it is dropped unanswered, as RFC 9112 allows. `request_started` restarts when leftover bytes remain
  after a response, so a partial pipelined request is still bounded by the header/body deadlines.
- **Producer streaming (`res_stream`).** The handler sets headers and calls `res_stream(res, producer, ctx, ctx_free)`:
  the chunked head goes into `out_buf` as for `res_write`, `res->stream_ended` is set (later `res_write`/`res_end` are
  no-ops) and `Connection.stream_fn/stream_ctx/stream_ctx_free` record the producer. The handler returns; the producer is
  never called inside it. `flush_connection`, each time `out_buf` has fully drained and `stream_fn` is set, frees an
  owned head tail-copy, lazily mallocs `stream_buf` and calls the producer with a `StreamWriter` over it
  (`cap = STREAM_CHUNK_SIZE - 5`, so the last chunk always fits). `stream_write` appends one framed chunk or refuses the
  whole write (-1). Then: `STREAM_MORE` sends and loops (same 4 × `STREAM_CHUNK_SIZE` per-flush fairness yield as file
  streaming); `STREAM_PAUSE` - or `STREAM_MORE` with nothing written, to rule out a busy loop - sets `stream_paused`,
  sends what was written, then `park_stream`: write interest dropped (a writable socket would otherwise call the producer
  in a tight loop), read interest (re)armed and `FLUSH_PENDING` returned; `STREAM_END` appends `0\r\n\r\n`,
  `stream_release`s (ctx freed), drains, frees `stream_buf`, and finishes like any response (keep-alive reset or close);
  `STREAM_ABORT` or any other value → `connection_close` without the last chunk, so the client sees a truncated body,
  never a clean end. `response_pending` includes `stream_fn != NULL`, so a paused stream still holds pipelined requests,
  counts as in flight for `app_stop` (`keep_alive = 0`, it may finish within the drain deadline), and is never treated
  as idle. Resuming (`resume_stream`: clear `stream_paused`, restart `last_write_progress`, watch write) happens from
  `app_wake_streams` and from `close_idle_connections`, which resumes every parked stream on each sweep instead of
  applying `WRITE_TIMEOUT_SECONDS` to it (the stall deadline still applies while a resumed stream has unsent bytes).
  Neither calls the producer directly: it runs on the next write-readiness event, so `app_wake_streams` is safe from
  inside another connection's handler. Read readiness on a live stream goes to `watch_stream_peer`: `recv(MSG_PEEK)` of
  one byte, EOF/error → close (frees the ctx immediately); real bytes (a pipelined request) → drop read interest and
  leave them in the socket until the stream ends. `stop_borrowing_read_buf` drops the already-dispatched request's
  bytes while a response is pending when nothing is pipelined behind it (`request_len = 0` then means "fully
  consumed"), so a long-lived subscriber holds no input buffer (the same applies to any pending response).
  Memory per streaming connection: one `STREAM_CHUNK_SIZE` buffer plus the application's ctx. MEASURED on macOS (one
  worker, `curl --limit-rate 2M`): the same 8.4 MB CSV took the server from about 1.5 MB to 38 MB RSS through
  `res_write` (and was truncated, see Known gaps), and stayed at 1.6 MB through `res_stream`. A 63 MB export also
  stayed at 1.6 MB.
- **Response safety.** Header names/values, trailers and cookie fields containing control characters are dropped
  (response-splitting defense); `res_redirect` with such a target answers 500. `Content-Length`, `Connection` and `Date` are engine-owned.
  Header and trailer values are never shortened: `set_named_value` (`response.c`) copies them whole into `conn->arena`
  (pointer + length in `ResponseHeader`); a name over 63 chars or a full table drops the entry (logged); a head that
  then exceeds `RESPONSE_HEADER_BUF_SIZE` drops the connection (logged). `res_redirect` answers 500 if its `Location`
  was not stored, and builds its `Redirecting to <location>` body in the arena, so a long target is not cut either.
- **Date and bodiless statuses.** Every head built by `build_response_head` (and the hand-built overload 503 in
  `connection.c`) carries `Date:` right after the status line, from `http_date_for(time(NULL))` (`http_parser.c`): one
  static per-process buffer reformatted only when the second changes (`format_http_date`, pure, locale-free, no
  `gmtime`). Safe only because each worker process is single-threaded. `100 Continue` has no `Date` (optional for 1xx).
  A 1xx / 204 / 304 status is bodiless on every sending path (`body_suppressed` in `response.c`): no `Content-Length` /
  `Transfer-Encoding` / `Trailer`, no default `Content-Type` (an explicit one is kept), no body bytes, no chunks, no
  file fd kept, and a `res_stream` producer's ctx is freed at once as for HEAD. Dropping the bytes is required, not
  cosmetic: without framing headers, any body byte would be read by a keep-alive client as the next response.
  `CookieOptions` zero value = session cookie; `max_age > 0` seconds, `< 0` expire now.
- **Accept path.** A new connection costs one syscall: `accept_client` is `accept4(SOCK_NONBLOCK | SOCK_CLOEXEC)` on
  Linux and plain `accept` on BSD/macOS, with no per-connection `fcntl`/`setsockopt`. It depends on inheritance from the
  listener, which `create_server_socket` makes non-blocking and sets `TCP_NODELAY` on. MEASURED: macOS inherits both
  `O_NONBLOCK` and `TCP_NODELAY`; Linux 6.8 inherits `TCP_NODELAY` but not `O_NONBLOCK`, hence `accept4` there, which also
  sets `FD_CLOEXEC` for free. `test_accept_client_socket_options` asserts the result on every platform, so a platform that
  stops inheriting fails a test instead of silently bringing back Nagle or blocking I/O. Both the per-worker path
  (`accept_connections`) and the single-acceptor master (`cluster.c`) use it. A passed fd keeps these options (they belong to the open
  file description), so `reject_overloaded_connection` no longer calls `set_nonblocking` either. No `SO_KEEPALIVE`: half-dead
  peers are already closed by the idle (60 s), request and write-stall deadlines, far sooner than TCP keepalive's
  default 2 h.
- **Overload.** `accept_connections` sheds load on two independent axes, both O(1), checked before
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
- **`Expect: 100-continue`.** When `serve_buffered_requests` finds a request's head complete but its body not,
  `send_continue_if_expected` (`connection.c`) writes `HTTP/1.1 100 Continue\r\n\r\n` straight to the socket, at
  most once per request (`Connection.continue_sent`, cleared with `body_limit_checked` in `flush_connection`'s
  keep-alive branch). The decision is the pure `request_head_expects_continue` (`http_parser.c`): HTTP/1.1+ only
  (never a 1xx to 1.0), a body to wait for (`Content-Length > 0` or chunked), valid framing, and an `Expect` value
  of exactly `100-continue`. Ordering: it runs after `reject_if_over_body_limit`, so an over-limit
  `Content-Length` gets `413` and the body is never invited; a head whose body already arrived is complete and never
  gets a 100. It is never queued in `out_buf`: the loop only reaches it with no response pending, so a pipelined
  request's 100 always follows the previous response. `EAGAIN` on that write is ignored (the client falls back to
  its own timeout, as it did before 100-continue support); a short write or hard error closes the connection, since half a status line
  would corrupt the stream. The route is not matched first: a 404/405 is still sent after the body, as Node does by
  default.
- **Body limits.** `app_use_body_limit(app, prefix, max_bytes)` (`router.c`) registers a `BodyLimitEntry` in
  `App.body_limits` (same segment-boundary prefix match as app-wide middleware; `max_bytes` clamped down to
  `MAX_BODY_SIZE`, never loosened past it). `connection.c`'s `reject_if_over_body_limit`, called from
  `handle_readable` right after every `recv` (before the completeness check), takes the `ParsedHead` that
  `handle_readable` already computed for this `recv` (it does not run its own `request_framing` pass -
  `Connection.body_limit_checked` still guards it running its actual check more than once per request, cleared
  with `request_started` in `flush_connection`'s keep-alive branch) and, as soon as headers are complete,
  looks up the limit with `app_body_limit_for_target` - the raw request-target through `request_target_path`, so the
  prefix sees the same canonical path as routing and middleware (a query string, `//` or `%75pload` no longer falls
  back to `MAX_BODY_SIZE`; a target the parser will refuse gets `MAX_BODY_SIZE` and its own 400/414 later). Longest
  matching prefix wins, independent of registration order; `MAX_BODY_SIZE` if nothing matches. The result is kept in
  `Connection.body_limit` for the rest of the request. A declared `Content-Length` over it is 413, sent before a
  single body byte is buffered or `in_buf` is grown. A chunked body is held to the same number two ways:
  `reject_if_chunked_over_body_limit` runs right after every `request_head_is_complete` and answers 413 once
  `chunk_scan.decoded_len` (validated chunks only) passes it, complete or not; and `grow_in_buf`'s raw-wire cap is
  `header_len + body_limit` instead of `+ MAX_BODY_SIZE`, which bounds a chunk declared huge that never finishes
  (raw bytes are never fewer than decoded ones). That raw cap only matters once `in_buf` outgrows `BUF_SIZE`, so a
  small limit still accepts a body of exactly its size framed in many tiny chunks.
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
  and open there (runs once per serving process, after fork). `SIGPIPE` is ignored per process in `app_listen_worker`
  (and, under `CEXPRESS_SINGLE_ACCEPTOR`, in the master too - see below). `cluster_listen` always calls
  `create_server_socket(app->config.bind_address, port)` once itself before forking anyone - this doubles as the respawn preflight validation.
  **Listen address.** Every listener comes from `create_server_socket(bind_address, port)`: `app_listen_worker`, the
  cluster preflight, and the single acceptor's `listen_fd`, so `ServerConfig.bind_address` reaches every process model.
  `NULL` = `0.0.0.0` (the historical, IPv4-only listener). Anything else must pass `inet_pton` as IPv4 or IPv6 (optional
  `%scope`) - hostnames and `inet_aton` shorthands like `127.1` are refused, so no DNS at startup and no name that
  resolves to several addresses - and is turned into a `sockaddr` by `getaddrinfo(AI_PASSIVE | AI_NUMERICHOST)`. An
  IPv6 listener gets `IPV6_V6ONLY` = 0 on every platform (BSD/macOS default to 1), so `::` also accepts IPv4. A refused
  address is a startup failure like a taken port: `-1`, and the caller exits once, before any fork.
  `create_server_socket` itself just `perror`s and returns `-1` on a bind/listen failure (fixed
  2026-09-23 - it no longer `exit()`s the process on its own); `cluster_listen` is what checks that return
  and `exit()`s with one clear message, so a fatal, permanent misconfiguration still stops the master
  before forking `workers_count` children that would all fail identically - MEASURED,
  10,594 respawns and 31,788 log lines in about 4 seconds with `WORKERS=2` and the port already taken.
  `app_listen_worker` (`connection.c`, the non-cluster/per-worker caller) checks the same return and exits
  the same way.
  **Single acceptor (`CEXPRESS_SINGLE_ACCEPTOR`, macOS/BSD only).** That one listen socket (`listen_fd`) is kept
  open, not closed, for the master's entire lifetime - it never binds a second one, and no worker binds any listen
  socket at all. Each worker instead runs `app_listen_worker_via_control_socket`, which points its `server_fd` at
  the worker-side end of a private `AF_UNIX SOCK_STREAM` socketpair with the master (`ClusterWorkerSlot.control_fd`
  is the master-side end) and sets `App.accept_via_fd_passing` - the event loop treats that fd exactly like a listen
  socket (`LOOP_EVENT_ACCEPT` still just means "`server_fd` is readable", unchanged across every `event_loop_*.c`
  backend), except `connection.c`'s `accept_passed_connections` drains it with `recvmsg`/`SCM_RIGHTS` instead of
  `accept_connections`'s `accept()`. The master's own supervision loop replaces its unconditional 50 ms
  `nanosleep` with a `poll()` on `listen_fd` of the same 50 ms timeout (adds no latency: `poll()` returns the
  instant a connection is pending, same backoff/`waitpid` cadence as before), `accept()`s everything pending each
  wake, and hands each fd to the next active worker round-robin (`dispatch_client_fd`, `cluster.c`), then closes its
  own copy either way: `SCM_RIGHTS` gives the worker its own reference, and while the master's stays open the worker's
  `close()` sends no FIN and the master leaks one fd per connection until `accept()` fails with `EMFILE`. A connection with
  no active slot available is closed with no response (best-effort shed, same philosophy as Overload, not
  duplicated here to keep this scoped). `set_nonblocking`/`TCP_NODELAY` are applied once, by the master right after
  its own `accept()`, and are never reapplied worker-side: both are file-status/socket-option properties of the
  underlying open file description, already in effect once the fd rides across via `SCM_RIGHTS` (same as across
  `dup()`/`fork()`). Both ends of every socketpair are set non-blocking too - the event loop's readiness contract is
  "drain until `EAGAIN`"; a blocking control socket's final `recvmsg` after draining everything pending would block
  forever instead, freezing that worker's entire single-threaded loop. The per-worker `max_connections`/
  `spare_fd` overload logic is untouched and still runs worker-side, now inside `accept_passed_connections` instead
  of `accept_connections` - it doesn't care how a client fd arrived. MEASURED: the
  pre-fix imbalance (over 90% of load on one worker of four) is gone - a live 4-worker run under `wrk -c5000` split
  requests dead evenly across all four (824/824/824/824 dispatched, 0 failures, instrumented count), and
  `scripts/stress_test.sh`'s peak-memory sampler moved from ~88% of total RSS on the largest single process to
  ~24%, matching the four-way split.
  **Respawn.** A worker that exits abnormally *after*
  startup (a real crash, not a bind failure) is respawned with exponential backoff per slot (100 ms, doubling, capped
  at 30 s) instead of instantly; if a slot fails more than `CLUSTER_RESTART_BUDGET` (5) times within
  `CLUSTER_RESTART_WINDOW_MS` (60 s) - a sliding window, not a lifetime count, so an occasional unrelated crash over a
  long-running server's life doesn't eventually trip it - the master gives up on the whole cluster, drains whatever
  workers are still up the same way a SIGTERM would, and `exit()`s non-zero itself (no error code returned to
  `app_listen`/`main()` - `cluster_listen` stays `void`; the *unchecked-allocation and forced-exit*
  half of this area was addressed, not `app_listen`'s own return-nothing contract, which remains a deliberate, larger, separate
  change - see `improvements_progress.md`). Both mechanisms are implementation details of `cluster.c` (the constants above
  are file-local, not in `app_types.h`, same convention as `connection.c`'s `ARENA_SIZE`) and share one accounting
  helper, `record_worker_failure`, so a `fork()` failure while trying to (re)spawn a slot counts against the same
  budget as an abnormal exit rather than looping unbounded on its own. Under `CEXPRESS_SINGLE_ACCEPTOR`, a respawn
  also creates a brand new socketpair for that slot every attempt (the dead worker's own end died with its process;
  the master's stale `control_fd` for that slot is explicitly closed first, or it would leak one fd per respawn) -
  the child closes every *other* slot's inherited master-side `control_fd` right after `fork()` (it must not be able
  to read or write a sibling's fd-handoff channel), keeping only its own.
- **Backend selection (Linux).** Both Linux backends are always compiled (unless `NO_URING=1`, which builds epoll
  only, without liburing). `event_loop_init` (`event_loop_linux.c`) opens epoll unless `CEXPRESS_EVENT_LOOP=io_uring`
  (see "Backend speed on Linux" for why epoll is the default). With io_uring requested there is no fallback: its init
  returns `EVENT_LOOP_UNAVAILABLE` (errno set) when `io_uring_queue_init` itself fails - `ENOSYS`, `EPERM` (seccomp,
  `kernel.io_uring_disabled`), `ENOMEM` (`RLIMIT_MEMLOCK`) alike - which the dispatcher logs with the errno before
  returning -1; any other io_uring setup failure is -1 too. `CEXPRESS_EVENT_LOOP=epoll` names the default, an empty value
  means unset, and anything else fails `event_loop_init` instead of guessing. The selection is stateless (a pure function
  of the environment variable and the build). The choice is `App.loop_ops`; every public `event_loop_*` forwards
  through it (one indirect call per event-loop operation, the same target for the process's life, so it predicts
  perfectly), and it is `NULL` whenever no loop is open, so `event_loop_is_open` never reads the union. The
  startup line names the backend: `Listening on port 8080 (epoll)`. MEASURED (Docker, Alpine, kernel 6.8, default
  seccomp): with io_uring requested, each worker exits at startup with the refusal logged; in cluster mode the master
  respawns it until the restart budget runs out (before 2026-09-23 that was also what happened by default).
  **Spurious `EINTR` after a ring is torn down:** once a process has closed an io_uring ring, the kernel may deliver
  task_work that makes its next `epoll_wait` return `EINTR` (seen on 6.8 when one test process ran io_uring then epoll).
  `event_loop_poll`'s contract already says to retry `EINTR` (`app_listen_worker` does); a serving process never
  switches backends, so it cannot hit this.
- **Event loop.** `Connection.events_watched` mirrors what the loop has registered. `connection.c` assumes **level-triggered**
  readiness from every backend: `read_and_serve` returns after serving and relies on read readiness firing again while
  bytes remain, and the fairness yields (`flush_connection`, `MAX_PIPELINED_PER_EVENT`) arm write interest on a socket
  that is already writable and rely on it firing. kqueue (default filters) and epoll (no `EPOLLET`) are level-triggered
  natively. io_uring polls fire on wakeups (edge), so `event_loop_io_uring.c` arms **one-shot** polls and re-arms each
  one right after its event (arming re-checks readiness, so a condition still true fires again next turn) - never
  multishot. `tests/test_event_loop.c` checks this contract on all three backends. All three backends skip an
  interest change that matches what is already registered (a keep-alive response then costs no extra syscall or SQE:
  `flush_connection` drops write interest after every response even when it was never set). kqueue and epoll decide
  from the tracked `events_watched` bit alone, so a Connection's bits must never claim an interest the kernel lacks
  (`connection_create` callocs them to 0; `unwatch_all` zeroes them). io_uring queues its SQEs and submits them once per
  `event_loop_poll`, combined with the wait (`io_uring_submit_and_wait`).
  **io_uring stale completions.** Each poll's user_data is `(generation << 32) | (fd + 1)`, the generation taken from
  `App.poll_regs[fd]` (`PollRegistration`: `gen`, `mask`, `armed`) and bumped on every arm or removal. A completion whose
  generation is not the fd's current armed one - the kernel's `-ECANCELED` for a removed poll, readiness a removed poll
  reported first, or anything from a previous connection on a reused fd number - is dropped. Before this, the removed
  poll's `-ECANCELED` was reported as `LOOP_EVENT_ERROR` and closed every keep-alive connection after its first request
  on Linux. Invariant: an fd is unwatched (`unwatch_all`) before it is closed - `connection_close` does so - or a reused
  fd number would inherit a stale "already armed" mask and never be polled.
  `event_loop_is_open(app)` is the only valid "is the loop up" test: `App.loop_fd` shares a union with the io_uring ring
  pointer and can read as negative while it is open (it did gate `app_stop`'s listen-socket unwatch and drain-deadline
  timer until the generation fix).
  `LOOP_EVENT_ERROR` (POLLERR/POLLHUP/POLLNVAL or a negative live completion) closes the connection.
  `LOOP_EVENT_WRITE` goes to `handle_writable`, not straight to `flush_connection`: it drains a pending response and then
  serves any pipelined requests still buffered. An `EAGAIN` mid-response also unwatches read and the keep-alive reset
  re-watches it (one extra `watch`/`unwatch` pair per response that did not fit the socket buffer in one go, none otherwise).
  **Backend speed on Linux (MEASURED 2026-09-23, Docker on an M3 Pro, kernel 6.8, one worker, `wrk -t8 -c1000`, cookbook
  `/hello`):** epoll served 3.5-3.6 M requests in 12 s, io_uring 2.8-2.9 M - io_uring is 20-25% slower as a pure readiness
  poller (one SQE and CQE per event on top of the same `recv`/`write`). So epoll is the default since 2026-09-24 and
  io_uring is opt-in (`CEXPRESS_EVENT_LOOP=io_uring`). It should become the default again only once it does the I/O
  itself (multishot `recv` with provided buffers, `send` SQEs) and measures faster. Not re-measured after the switch.

## Hot-path rules (measured; do not undo)
Per-request CPU cost of the pure path (parse, route, dispatch, response build; no sockets, one core; `make bench`, Apple M3 Pro,
gcc-16 -O2, 22 Sep 2026): minimal GET 160 ns, browser-shaped GET (10 headers, cookies, query) 400 ns, JSON POST 195 ns, 404 170 ns
(single-run noise is ±15-25 ns at this scale, `make bench` re-run several times); a 20-row JSON list through yyjson 700 ns.
`bench_hotpath.c` also reports a second browser-shaped case where the handler actually calls `req_get_cookie` +
`req_get_header` (the case lazy header/cookie parsing can't help, since the handler reads them anyway): ~480 ns - still faster than the
earlier 514 ns baseline, since header storage itself got cheaper independent of whether a handler reads one.

History, most recent first (only ratios transfer between machines; each line is the same four/five cases in the same
order as above): 180 / 514 / 235 / 180 ns, before `req->headers` changed from fixed-size copies to views into `in_buf`
(materialized lazily by `req_get_header`) and cookie splitting became lazy (`req_get_cookie`, on its first call per request)
instead of eager in `parse_http_request_from_head` - **MEASURED** browser-shaped GET a further −22%, minimal GET −11%,
JSON POST −17%, 404 −6%, roughly matching the PROJECTED "further ~200 ns (browser-shaped)" (514 → 400 ns
here is 114 ns, in the same range accounting for machine/run variance). Before that, 198 / 825 / 313 / 208 / 709 ns, before
`bench_hotpath.c`'s `one_request` switched to calling `parse_request_head` + `request_head_is_complete` +
`parse_http_request_from_head` - one picohttpparser pass, matching the real `connection.c` hot path - instead of the
separate `request_is_complete` + `parse_http_request` it called before, which cost two passes even after the single-pass change's own internal
dedup of `parse_http_request` - **MEASURED** browser-shaped GET −38%, JSON POST −25%, matching the
PROJECTED estimate almost exactly; minimal GET and 404 move less because they carry only 1-2 headers, so there is less
redundant tokenizing to remove. Before that, 208 / 781 / 315 / 186 / 659 ns on 21 Sep 2026, before raising
`Request.header_values`' per-slot size from 256 to 1024 (header views later deleted this array and the cap entirely - see "Behavior
reference, Request parsing") - that delta was noise, not attributable to the change. The earliest version of this file recorded
390 / 800 / 470 / 430 ns for the handwritten-parser engine on the same machine (no A/B rebuild of that commit was done for
this update). What to keep:
- No `strtok_r` / `sscanf` / `strncpy` (zero-pads to the full size) / `strcasestr` over request bytes. Scan with lengths and `memchr`.
- No whole-struct `memset` of `Request` (9,792 bytes, `sizeof`, `make bench` - down from 43,576 bytes when header views replaced
  `header_names`/`header_values[32][1024]` with `struct phr_header headers[32]`, 1,024 bytes of views instead of a
  34,816-byte fixed-size copy; remaining bulk is `cookie_names`/`cookie_values` (5 KB) and `query_names`/`query_values`/
  `param_names`/`param_values` (3 KB), still fixed-size copies, deliberately out of scope - see "Known gaps") or
  `Response` (16 KB). `parse_http_request_from_head` and `res_init` set scalars and
  `*_count` only; arrays are read up to their count and every slot is NUL-terminated on write.
- One `phr_parse_request` pass per request on the hot path, not up to four: `handle_readable` calls `parse_request_head`
  once and threads the result through the body-limit check, `request_head_is_complete` and `parse_http_request_from_head`.
  Don't reintroduce a second call to `request_framing` / `request_is_complete` / `parse_http_request` (the whole-buffer
  re-parsing wrappers) anywhere in `connection.c`'s per-`recv` loop - they exist for callers that only need one piece
  (tests, `fuzz_parser.c`) and each costs its own independent pass again.
- Don't copy header names/values into `Request`: `parse_http_request_from_head` stores views (`req->headers[i] =
  head->headers[i]`, a struct assignment of two pointers and two `size_t`s) instead of `copy_bounded`-ing each one into
  a fixed-size slot. A request whose headers nobody reads should cost nothing beyond that assignment; don't reintroduce
  a per-header `memcpy` there. Don't call `parse_cookies` from `parse_http_request_from_head` either - `req_get_cookie`
  triggers it lazily, once, on its own first call per request.
- Don't copy the request body: `connection.c` parses with `parse_http_request_in_place`, so `req->body` is a view into
  `in_buf` (chunked decoded in place). Don't switch it back to `parse_http_request_from_head` (a full body `memcpy` into the
  arena, a second 10 MiB block for a maximum-size upload), and keep the saved-byte restore right after `dispatch`.
- Routing is one tree walk over path segments with no allocation; `req == NULL` searches without capturing (used for the 405 `Allow` list).
- A Patricia node's static `children` stay sorted; find/insert through `find_child` (binary search), not a linear scan ("Behavior reference, Routing" above).
- Response head is assembled with bounded `memcpy` appends and an integer formatter, not `snprintf`.
- Allocate per-request data from `conn->arena`, not `malloc`. Emit JSON through yyjson with `arena_yyjson_alc`.
- No syscall on a path that changes nothing (`events_watched`; enforced on every backend, see Event loop).
Verification tools: `make bench`, `make test` (13 suites), `make SANITIZE=1 BUILD_DIR=build-asan test` (ASan + UBSan),
`make fuzz` (mutation fuzzer over parser/router/response, then an end-to-end "every input is answered or closed" check through the real connection code), `make check-docs`. Run sanitizers and fuzz after touching
`http_parser.c`, `router.c`, `response.c` or `arena.c`. The Makefile tracks header dependencies (`-MMD`).

## Known gaps (verified, not fixed)
- **Chunked framing overhead is not capped separately**: 10 MiB of 1-byte chunks is accepted up to the raw `MAX_BODY_SIZE` wire cap. The scan is linear (~14 ms for that worst case, MEASURED), so this is a cost bound, not an amplification.
- Path/query params over 63 chars and queries over 255 chars are truncated silently. So are individual cookie values
  over 255 chars after the `Cookie` header is split (`parse_cookies` → `cookie_values[MAX_COOKIES][256]`) - the raw
  `Cookie:` header line itself has no length limit any more (it's a view like every other header, materialized in
  full by `req_get_header`/`req_get_cookie`), but a single very long session-token cookie among several shorter ones,
  once split out of that line, can still be truncated to its own 255-byte slot. Request header values otherwise are
  not truncated at all any more (views, not fixed-size copies) - see "Behavior reference, Request parsing". Query
  and path-parameter storage stayed fixed-size copies deliberately, out of scope: `req->query_names`/`query_values`
  is a small, session-eager array (embedded-NUL rejection requires validating every value for an embedded NUL at parse time regardless of
  whether a handler ever reads it, so there is no CPU to save by deferring the copy, only Request's overall size - and
  query/param storage together are under 3 KB, a small fraction of what headers used to cost).
- **Multiple `res_send` calls, chunked growth and static files leave dead copies in the arena** until the request ends (see Memory model). For large bodies use `res_stream`, which never touches the arena past the head.
- **`res_write` silently truncates at its cap** (MEASURED 2026-09-23 while measuring `res_stream`): `append_to_out_buf` refuses anything past `MAX_BODY_SIZE + 8 KiB` of *wire* bytes, chunk framing included, and `res_write` ignores the failure. A CSV of 450,000 short lines (8,426,423 body bytes, about 11 MB framed) went out as 7,947,523 bytes with status 200 and no error anywhere. The handler cannot tell. `res_stream` has no such cap.
- **A parked `res_stream` producer whose client vanished silently is only noticed on its next write**. A FIN/RST is caught at once (`watch_stream_peer`), but once a pipelined request has arrived behind the stream, read interest is dropped and only a write can fail. A producer that parks indefinitely without ever writing holds its connection and ctx until shutdown; long-lived streams should write a heartbeat (an SSE comment line, `:\n\n`) every so often - the idle sweep calls them about once a second, so they can check the time.
- **`app_wake_streams` is O(connection table) and wakes every paused stream on the worker**, not a channel's subscribers; producers with nothing new just park again. It is per process: in a cluster, a publish reaches only the worker that handled it.
- The io_uring backend is used only as a readiness poller; sockets are still read and written with `recv` / `write`.
- **wrk against Linux in Docker reports "timeout" counts close to the connection count** (for example 900-1650 at
  `-c1000`), on epoll and io_uring alike, with max latency in milliseconds and none on macOS/kqueue. Same size either
  backend, so not an engine-backend defect; not explained (see `improvements.md`).
- No HTTP/2, compression, `Range`, or WebSocket. `Expect` values other than `100-continue` are ignored (no `417`).
- **`res_send_file` and large (uncached) static files are still not optimized** (`improvements.md`, the static-file item's other sub-items,
  not addressed by the static-file cache above): the response head and the file body still go out as separate `write`
  calls (no single buffer / `writev`), and large files are read with plain `read`/`write` in `STREAM_CHUNK_SIZE` pieces
  rather than `sendfile(2)`. The write-stall deadline fires only on zero progress, so a client reading a large file a
  byte at a time holds its fd and 16 KiB `stream_buf` indefinitely (no minimum-rate rule). No `ETag`/`Last-Modified`/`304`/`Cache-Control` on any response, static or otherwise.

## Where to change what
Add a response helper → `response.c/h` + `tests/test_response.c` + `API.md`. Change producer streaming (`res_stream`, parking, waking) → `response.c` + `connection.c` (`flush_connection`, `park_stream`/`resume_stream`, `watch_stream_peer`) + `tests/test_stream.c`. Add a parser feature → `http_parser.c/h` +
`tests/test_http_parser.c` (or `test_http_hardening.c` for a bug regression) + a case in `tests/fuzz_parser.c` seeds. Add a route feature → `router.c/h` + `tests/test_router.c`.
Add middleware behavior → `middleware.c` + `tests/test_middleware.c`. New public function → declare it in the header and list it in
`API.md` (`make check-docs` enforces this). New recipe → `examples/cookbook.c` + `tests/test_cookbook.c`. Anything allocated per request → the arena.
