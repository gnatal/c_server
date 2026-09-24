# Tradeoffs: the performance-oriented engine changes

This document records the architectural decisions behind the engine's performance work, what each one costs, and what was measured. It covers five changes, in the order they landed: the per-connection arena, yyjson, picohttpparser, the Patricia-tree router and the io_uring event loop. Measurements are from an Apple M3 Pro, gcc-16 -O2, 21 Sep 2026, and are single runs unless noted. Nothing here was measured on Linux.

## 1. Per-Connection Arena Allocator (Bump Allocator)

`lib/arena.c` provides a bump allocator that owns everything allocated while serving one request.

### The Decision
`connection_create` does one `calloc(1, sizeof(Connection) + 64 KiB)`; the arena's buffer is the 64 KiB right behind the struct, so a connection and its arena are one allocation and one `free`. Separately, the 8 KiB input buffer `in_buf` is a normal `malloc` (it must grow with the request body). `arena_alloc` rounds to 8 bytes and bumps an offset. `flush_connection` calls `arena_reset` once a keep-alive response is fully written; `connection_close` calls `arena_destroy`.

The arena holds `Request.body`, the response head and body (`conn->out_buf`), the file-streaming chunk buffer, chunked-response growth, and yyjson documents (through `arena_yyjson_alc`). Per-request bookkeeping that does not need to outlive the handler therefore has no matching `free` to forget.

### Pros
* **Cheap allocation**: pointer arithmetic, no free-list search.
* **No fragmentation and no per-allocation leaks** for anything routed through the arena: `arena_reset` reclaims it all, including the malloc'd fallback blocks.
* **Simpler error paths**: parsing failures, early middleware returns and handler errors need no cleanup of request memory.

### Cons / Tradeoffs
* **Memory footprint per connection.** Allocated per connection: 72 KiB (64 KiB arena + 8 KiB `in_buf`) plus the `Connection` struct. Measured resident cost on macOS with 5,000 idle keep-alive connections on one worker: about 121 MB, or about 25 KB per connection, whether the connections were idle or had each served one `GET /ping`. That is well below the 72 KiB allocated, presumably because pages of the `calloc`'d arena that are never written are not resident; that mechanism was not verified, and **Linux was not measured** (a recycled glibc heap chunk is zeroed by `calloc`, so expect a larger resident figure there). The engine before the arena measured about 7 KB per connection. An earlier version of this document estimated "10,000 idle connections = ~640 MB"; the measured macOS figure for that count would be on the order of 250 MB resident, with about 720 MB allocated.
* **Fallback is not limited to big requests.** When the *remaining* space cannot hold an allocation, `arena_alloc` `malloc`s it into a linked list of blocks that `arena_reset` frees. A response that is large in total (a 200 KB JSON list, a chunked stream up to the 10 MiB cap) pays for `malloc` and for the copy, and a static file up to 50 MiB is read into a `malloc` buffer and then copied again into the arena by `res_send_bytes`.
* **No `free` or in-place `realloc`.** Growth allocates a new region and copies (`append_to_out_buf` doubles this way; yyjson's realloc hook does too), and the old region stays in the arena until the request ends. A second `res_send` in the same handler likewise leaves the first response's bytes behind.
* **Lifetime rules leak into application code.** Anything reached through `req` or `res`, and any arena-backed yyjson document, dies when the response has been written. A handler that stashes such a pointer in a global reads freed-then-reused memory on the next request. See `lib/CLAUDE.md`, "Memory model".
* **`Request.body` is still a copy** of the input buffer (now into the arena). Zero-copy would need the parser to hand out NUL-terminated slices of `in_buf`.

## 2. Replacing the In-House JSON Library with `yyjson`

### The Decision
The in-repo parser, tree builder and streaming `JsonWriter` (`lib/json/`, `tests/test_json.c`) were deleted in favour of [yyjson](https://github.com/ibireme/yyjson) 0.13.0, vendored in `lib/vendor/yyjson/` and included by `cexpress.h`. Handlers create documents with `arena_yyjson_alc(&res->conn->arena)`, so building or parsing a document allocates only from the arena.

### Pros
* **Speed and coverage**: yyjson is a widely used, heavily tested library; emitting a 20-row todo list takes about 660 ns in `make bench` (with the default allocator). For scale, the previous docs recorded about 15 µs for the old tree builder on the same 20 rows; the `JsonWriter` figure was not recorded, so no comparison with it exists.
* **Arena synergy**: with `yyjson_alc` pointed at the arena, a document's nodes and strings cost bump allocations and no cleanup.
* **Correctness**: string escaping and 64-bit integers are handled by a maintained library.

### Cons / Tradeoffs
* **A trap in the ownership story.** `yyjson_mut_write(doc, 0, &len)` returns a string from libc `malloc` regardless of the document's allocator, so the caller must `free` it. Every JSON-emitting handler in `examples/todo_sqlite/` and the cookbook does; forgetting it leaks one string per request. (`yyjson_mut_write_opts` accepts an allocator, which would put the string in the arena too; the repo does not use it.)
* **External code in the tree**: `lib/vendor/yyjson/` is over 19,000 lines and a larger binary. It is exempt from the project's 1,000-line file limit only because it is vendored.
* **A more verbose API** than the in-house builder: separate mutable and immutable document types, and `yyjson_mut_obj_add_*` calls that return `bool` (the repo's code does not check them).
* **DOM memory**: a document is fully materialized. For very large payloads that costs more than streaming would (the parser is the DOM kind, not SAX). Request bodies are capped at 10 MiB.
* **Borrowed strings**: `yyjson_mut_obj_add_str` stores the pointer, not a copy; the source must outlive the write.
* **Lost test coverage**: the deleted `test_json` suite is gone; JSON is now exercised only through `test_cookbook` and the demo.

## 3. PicoHTTPParser

### The Decision
The handwritten request-line and header parser was replaced by [picohttpparser](https://github.com/h2o/picohttpparser), vendored in `lib/vendor/picohttpparser/` (its SIMD path is SSE4.2 only, so on Apple silicon and other non-x86 targets it runs the scalar code). `http_parser.c` keeps everything around it: framing (`Content-Length`, chunked), the fixed-size `Request` arrays, cookies, queries, percent-decoding.

### Pros
* **A well-exercised tokenizer** for the request line and headers, and speed (measured here without its SIMD path, see above): `make bench` shows 208 ns for a minimal GET and 781 ns for a browser-shaped GET (10 headers, cookies, query) through the whole pure path; the previous docs recorded about 390 and 800 ns for the handwritten parser on the same machine (not re-measured on the old commit).
* Smuggling protection stayed in the engine's own framing code: `Content-Length` together with chunked encoding, and conflicting duplicate `Content-Length` headers, are still a 400.

### Cons / Tradeoffs
* **It rejects what it does not like, and the engine does not always answer.** picohttpparser accepts HTTP/1.x only and treats a malformed request line as an error. `request_framing` reports that as "no header block", and `request_is_complete` reads it as "incomplete", so a request such as `GET /\r\n\r\n` or `GET / HTTP/2.0` gets **no response** and the connection stays open until 8 KiB arrive or the 60 s idle timeout fires. Verified against a running server on 21 Sep 2026 (see `lib/CLAUDE.md`, "Known gaps"). An earlier version of this section said such clients "will be outright rejected"; today they are silently held.
* **Not strictly `\r\n`.** Bare `\n` line endings are accepted (verified).
* **A hard header cap**: the parser is handed a 32-slot array (`MAX_HEADERS`); a 33rd header makes the request a 400 (verified). Before, extra headers were dropped.
* **The header block is parsed three times per request**: once in `request_is_complete` (`request_framing`), then in `parse_http_request` and again by the `request_framing` call inside it. Collapsing that is a possible optimization.

## 4. Patricia-Tree Router

### The Decision
Routes used to live in one flat array searched linearly (32 routes maximum). Each HTTP method now has its own tree of `PatriciaNode`s (`app_types.h`, `router.c`). Despite the name, a node is a whole path segment, not a compressed character prefix: it is a segment trie. Matching walks the request path once, trying at each node a literal child, then the `:param` child, then a trailing `*` catch-all, and backtracks when a branch fails.

### Pros
* **No fixed route cap** on an `App` (64 per `Router` before `app_mount`); lookup cost depends on path depth and fan-out, not on the number of routes.
* **Specificity beats registration order**: `/users/me` wins over `/users/:id` even when registered second. Previously the first registered match won.
* **Allocation-free matching**, with `req == NULL` used for the 405 `Allow` computation.

### Cons / Tradeoffs
* **Parameter names belong to the tree position, not the route.** `/orders/:id/items` and `/orders/:oid/notes` share one parameter node named after whichever was registered first, so `req_get_param(req, "oid")` returns `NULL` (verified). A route registered after a mid-pattern `*` at the same position captures nothing.
* **Heap-allocated routes**: one `Route` (1,368 bytes on macOS, where `PATH_MAX` is 1024; about 4.4 KB on Linux, where the static-root buffer is 4096)  and its nodes are `malloc`'d per registration, and freed by `app_free_routes` from `app_destroy`. The unit tests in `tests/test_router.c` do not call it, so they leak route trees.
* **Recursive search** bounded by the path-segment count (a 255-byte path).
* Up to 16 distinct HTTP methods per app.
* The old `match_path` matcher is kept for tests but the router no longer calls it, so the two can drift.

## 5. io_uring Event Loop on Linux

### The Decision
On Linux the Makefile selects `lib/event_loop_io_uring.c` (liburing) in place of epoll. It is used **only as a readiness poller**: each watched fd gets a multishot `POLL_ADD`, completions are translated to `LoopEvent`s, and sockets are still read with `recv` and written with `write`. `signalfd` and `timerfd` cover signals and timers. `lib/event_loop_epoll.c` remains in the tree, built when `CEXPRESS_USE_EPOLL` is defined (macOS `make test_epoll` through epoll-shim), but the Linux Makefile does not pick it.

### Tradeoffs
* **No benefit has been measured in this repository.** There is no epoll-versus-io_uring comparison, and the Linux build was not run for this update. The io_uring code path only became testable through Docker (`scripts/docker_stress_test.sh`).
* **Dependencies and deployment**: needs liburing to build and a kernel with multishot poll (5.13+) at run time. If `io_uring_queue_init` fails, for example because a container's seccomp profile blocks it, the process exits; there is no fallback to epoll (`concurrency.md`, section 3D).
* **More syscalls than kqueue on interest changes**: the kqueue backend skips a syscall when the registered state already matches (`events_watched`); the io_uring backend submits a poll-remove plus a new poll on every `watch_*` / `unwatch_*`, including the `unwatch_write` after every keep-alive response. Adding the same early-out is the obvious next step.
* Not using io_uring's actual submission path for `recv` / `write` leaves most of its potential unused.

### Outcome (2026-09-24)
The first comparison (2026-09-23, Docker on the M3 Pro, kernel 6.8, one worker, `wrk -t8 -c1000`) measured epoll at 3.5–3.6 M requests in 12 s against io_uring's 2.8–2.9 M, so io_uring as a pure readiness poller is 20–25% slower. Both backends are now built on Linux, and `event_loop_init` runs **epoll by default**. io_uring is opt-in with `CEXPRESS_EVENT_LOOP=io_uring` and has no fallback. It should become the default again only once it does the socket I/O itself (multishot `recv` with provided buffers, `send` SQEs) and measures faster. See `lib/CLAUDE.md`, "Backend selection (Linux)".

## Conclusion
Together, the arena, yyjson, picohttpparser and the segment-tree router remove `malloc`, linear scans and hand-written tokenizing from the hot path: `make bench` measures 0.19 to 0.78 µs per request for the pure path (parse, route, dispatch, response), and an end-to-end `wrk` run against the demo (single runs, 4 workers, `wrk` on the same machine) shows about 250k req/s on `GET /ping` at 100 connections, 69k on `GET /api/todos` (20 rows, SQLite) and 60k on `GET /` (6.5 KB file), all at 100 connections. The bill is paid in three places: a much higher baseline memory per connection (about 25 KB resident on macOS versus about 7 KB), lifetime rules that application code must respect, and behavior changes that the tests did not catch (silent handling of malformed request lines, route-parameter naming). See `lib/CLAUDE.md`, "Known gaps".
