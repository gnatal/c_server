# Improvements: performance (P), memory management (M), security (S)

Review of the engine in `lib/` as of commit `658ac6b` (2026-09-23). `examples/` was out of scope.
Items already listed under "Known gaps" in `lib/CLAUDE.md` are not repeated, except where this review
found new evidence or a cheaper fix. Items fixed in the previous round (S1–S12, P1–P10, M1–M6, C1–C6)
were checked and are not reopened.

**Evidence labels.** MEASURED: reproduced on this machine (macOS, Apple M3 Pro, gcc-16 -O2), or in
Docker (Alpine, Linux 6.x, epoll) where noted. CODE: found by reading the code; not run.
ESTIMATED: the gain is an estimate, not a measurement.

**Effort.** S = up to half a day, including the regression test. M = 1–2 days. L = 3 days or more.
Every fix needs a test in `tests/` (`CLAUDE.md` rule). The "How to reproduce" steps are the starting
point for that test.

Most repro steps use a small probe server, listed in [Appendix A](#appendix-a-probe-server-used-for-the-reproductions).

---

## Summary

| ID | Problem | Impact | Effort | Evidence |
|---|---|---|---|---|
| **S1** | Prefix middleware (auth) is bypassed with `//admin/...` or `/%2Fadmin/...` | **High** | S | MEASURED |
| **M1** | macOS cluster master never closes the fds it hands to workers | **High** (cluster stops accepting; no FIN) | S | MEASURED |
| **S2** | `app_use_body_limit` is bypassed by a query string, encoding, `//`, or chunked encoding | High | S | MEASURED |
| **M2** | Large static files: whole file read and copied twice; slow readers pin the full size each | High (memory DoS) | S | MEASURED |
| **S3** | Chunk-size parsing accepts `0x5`, `+5` and ` 5` (request smuggling behind a proxy) | Medium | S | MEASURED |
| **S4** | Static mounts serve dotfiles (`.env`, `.git/config`) | Medium | S | MEASURED |
| **S5** | Multipart: quoted parameters parsed wrongly; `filename` returned with `../` | Medium | S | MEASURED |
| **S6** | Server always binds `0.0.0.0`, so a proxy-only deployment is exposed directly | Medium | S | CODE |
| **P1** | epoll issues one wasted `epoll_ctl` per keep-alive request | Medium (~25% of syscalls) | S | MEASURED |
| **P2** | io_uring is the Linux default but is 20–25% slower than epoll | Medium | S | MEASURED (`lib/CLAUDE.md`) |
| **P3** | Incomplete headers are re-parsed from byte 0 on every `recv` (quadratic) | Medium (CPU DoS amplifier) | S–M | MEASURED |
| **M3** | No per-worker memory budget for buffered bodies and unsent output | Medium | M | CODE |
| **S7** | Missing or duplicate `Host` header is accepted | Low–Medium | S | MEASURED |
| **S8** | Form fields: `%00` not rejected (S6 gap), values silently truncated at 255 | Low–Medium | S | MEASURED |
| **S9** | Response header values silently truncated at 255 chars (CSP, `Location`) | Low–Medium | S | MEASURED |
| **P4** | Cached static files are copied into the arena on every hit | Low–Medium | M | ESTIMATED |
| **P5** | `sendfile`/`writev` still not used for file bodies | Low–Medium | M | CODE |
| **P6** | One unnecessary syscall per connection close (`unwatch_all` before `close`) | Low | S | CODE |
| **M4** | Arena growth always copies (yyjson realloc, `res_write` doubling) | Low–Medium | S | CODE |
| **M5** | Static cache: `./` aliases create duplicate entries; 64 MiB per worker | Low | S | CODE |
| **M6** | A partial request always costs a full 8 KiB owned buffer | Low | S | CODE |
| **P7** | Static cache lookup is a linear `strcmp` scan over up to 256 entries | Low | S | CODE |
| **S10** | `arena_alloc` has no size-overflow check | Low (hardening) | S | CODE |

**Suggested order:** S1, M1, S2 and M2 first (each S effort, each confirmed). Then S3, S4, S5, P1 and
P2, which are also small. Then P3, S6–S9, M3. The rest when convenient.

S1 and S2 share a root cause and one fix: **canonicalize the request path once, and match every prefix
(middleware, body limits, static mounts) on segments, exactly as the router does.**

---

## P — Performance

### P1 · epoll issues a wasted `epoll_ctl` on every keep-alive response
- **Impact:** Medium. About 1 of 4 syscalls per request on the Linux backend that measured fastest.
- **Effort:** S
- **Where:** `lib/event_loop_epoll.c:263` (`epoll_unwatch_write`), called unconditionally by `flush_connection`'s
  keep-alive branch at `lib/connection.c:654`. `epoll_unwatch_read` (`:199`) has the same shape.
- **Problem:** `epoll_unwatch_write` clears the bit and then always calls `epoll_ctl(MOD, EPOLLIN)`, even when
  `EVENT_WRITE` was never set. kqueue and io_uring already skip no-op changes (`events_watched`). This is the
  still-open "epoll half" of the old P4.
- **How to reproduce (MEASURED, Docker/Alpine, epoll build):** run a server under `strace -f -c`, then send 1,000
  keep-alive requests on one connection:
  `curl -s $(for i in $(seq 1000); do printf 'http://127.0.0.1:18080/static/a.txt '; done) >/dev/null`.
  Result: `write` 1000, `recvfrom` 1001, `epoll_pwait` 1004, **`epoll_ctl` 1007**. That is 4 syscalls per
  request where 3 would do.
- **Fix:** At the top of `epoll_unwatch_write`/`epoll_unwatch_read`, return 0 when the connection is known and
  the bit is already clear, as `event_loop_kqueue.c:watch/unwatch` do. Assert the syscall count in a Linux-only
  test (for example with `test_event_loop.c` and a counter behind the ops table).
- **Expected gain:** ESTIMATED 5–15% keep-alive throughput on epoll (one of four syscalls removed; `epoll_ctl`
  was ~20% of syscall time in the trace).

### P2 · Linux defaults to io_uring, which measured 20–25% slower than epoll
- **Impact:** Medium
- **Effort:** S (switch the default) or L (make io_uring do real I/O)
- **Where:** `lib/event_loop_linux.c` (tries io_uring first). The measurement is in `lib/CLAUDE.md`, "Backend speed on Linux".
- **Problem:** io_uring is used only as a readiness poller (one `POLL_ADD` SQE and one CQE per event, then
  ordinary `recv`/`write`), so it adds work on top of what epoll does. The docs measured epoll at 3.5–3.6 M
  requests against io_uring's 2.8–2.9 M (`wrk -t8 -c1000`, 12 s), yet io_uring remains the default.
- **How to reproduce:** `CEXPRESS_EVENT_LOOP=epoll` vs `CEXPRESS_EVENT_LOOP=io_uring`, same binary, same `wrk` run.
- **Fix:** Make epoll the default and keep io_uring behind `CEXPRESS_EVENT_LOOP=io_uring`. Do this together
  with P1. Only move io_uring back to default once it does the I/O itself (multishot `recv` with provided buffers,
  `send` SQEs), which is an L effort.

### P3 · Incomplete request heads are re-parsed from the start on every `recv` (quadratic)
- **Impact:** Medium. The CPU cost of a slowloris connection is quadratic in the head size. It's bounded by
  `BUF_SIZE` (8 KiB) and the 10 s header deadline, but still about 1,000–2,000× the normal cost.
- **Effort:** S (pre-check) or M (true incremental parse)
- **Where:** `lib/connection.c:856` (`parse_request_head` on every `serve_buffered_requests`),
  `lib/http_parser.c:211` (`phr_parse_request(..., last_len = 0)`), `lib/http_parser.c:216` (`has_blank_line`, a
  second full scan on the incomplete path).
- **How to reproduce (MEASURED):** call `parse_request_head` on each prefix of an 8,157-byte head that has no
  terminating blank line (the probe `quad.c` in Appendix A):
  - byte-at-a-time: **13–21 ms CPU** per connection
  - one call per 1,460-byte segment: **0.006–0.012 ms**

  A few hundred connections dripping one byte per packet keep one worker core busy.
- **Fix (S):** keep a `Connection.head_scan` offset and look for the end-of-head blank line incrementally,
  resuming 3 bytes back as `chunked_body_scan_resume` does with `trailer_from`. Call picohttpparser only once a
  blank line exists. **(M)** Alternatively, pass picohttpparser's `last_len` so it resumes.

### P4 · Every cached static hit copies the whole file into the arena
- **Impact:** Low–Medium (hurts files between ~64 KiB and 256 KiB most)
- **Effort:** M
- **Where:** `lib/static.c:281` and `:336` → `res_send_bytes` → `lib/response.c:249-259` (`arena_alloc` + `memcpy`).
- **Problem:** The cache holds the bytes, but each hit copies them into `conn->out_buf`. Past the 64 KiB arena
  that copy is a `malloc` plus `free` per request (the arena's fallback path). A partial write then makes a
  third copy (the `EAGAIN` tail copy, `connection.c:515`).
- **Fix:** Send the head from the arena and the body straight from the cache entry, with `writev` on the first
  attempt. If it doesn't all go out, keep a reference to the entry (refcount it so eviction can't free it
  while it's in use) instead of copying the tail.
- **Expected gain:** ESTIMATED. Removes one `malloc`/`memcpy`/`free` of up to 256 KiB per hit.

### P5 · File bodies still use `read` + `write` and a separate head `write`
- **Impact:** Low–Medium
- **Effort:** M
- **Where:** `lib/connection.c:540-586` (file streaming loop), `lib/response.c:533` (`res_send_file`).
- **Problem:** Already listed in `lib/CLAUDE.md` (Known gaps). It matters more now that M2 recommends routing
  large static files through this path. Each 16 KiB chunk is copied kernel→user→kernel, and the head goes out
  in its own `write`.
- **Fix:** `sendfile(2)` (Linux: `sendfile(out, in, &off, n)`; macOS: `sendfile(in, out, off, &len, hdtr, 0)`,
  which can carry the head in `hdtr`). Keep `read`/`write` as a fallback.

### P6 · One unnecessary syscall per connection close
- **Impact:** Low (larger for `Connection: close` or HTTP/1.0 traffic such as health checks)
- **Effort:** S
- **Where:** `lib/connection.c:142` → `lib/event_loop_kqueue.c:150` (`kevent(EV_DELETE × 2)`) /
  `lib/event_loop_epoll.c:289` (`EPOLL_CTL_DEL`).
- **Problem:** `close(fd)` already removes kqueue knotes, and epoll registrations for an fd with no other
  references. The explicit delete just before `close` is an extra syscall.
- **Fix:** On kqueue and epoll, `unwatch_all` only resets `events_watched`. Keep the real removal for io_uring,
  whose generation tracking (C6) depends on it. The old P10 measured 7 syscalls per `Connection: close` request
  on Linux, so this saves about 1 in 7 (ESTIMATED).

### P7 · Static cache lookup is a linear `strcmp` over up to 256 entries
- **Impact:** Low
- **Effort:** S
- **Where:** `lib/static.c:177` (`cache_find`), used on every static request, and twice on a revalidation.
- **Fix:** Store a 64-bit hash (FNV-1a) of the key in each entry and compare hashes before calling `strcmp`,
  or use a small open-addressing table. `cache_evict_stalest` can stay O(n); it runs only on insert.

---

## M — Memory management

### M1 · macOS cluster: the master leaks every client fd it passes to a worker
- **Impact:** **High**, on macOS/BSD with `workers > 1` (`CEXPRESS_SINGLE_ACCEPTOR`)
  - **Leak:** one fd per connection, forever.
  - **Broken close:** a worker's `close()` never sends a FIN, because the master still holds the socket.
    `Connection: close` responses and idle timeouts do not end the TCP stream.
  - **Outage:** once the master reaches `RLIMIT_NOFILE`, it cannot `accept()` and the whole cluster stops
    taking connections.
- **Effort:** S (one line, plus a test)
- **Where:** `lib/cluster.c:480-488`. After `accept_client`, `client_fd` is closed only when
  `dispatch_client_fd` fails. `SCM_RIGHTS` gives the worker its own copy, so the sender must close its copy.
- **How to reproduce (MEASURED):** `WORKERS=2 PORT=18081 ./probe`, then:
  - After 300 `curl` requests: `lsof -p <master> | grep -c TCP` → **302** (1 listen socket + 301 leaked);
    the workers hold 0.
  - `printf 'GET /static/a.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' | nc -w 5 127.0.0.1 18081`
    only ends at nc's 5 s timeout (**5.01 s**). The same request to a single-process server ends in **0.01 s**.
  - With `ulimit -n 64`, 57 of 80 sequential requests succeed; **every request after the 57th fails**.
- **Fix:** `close(client_fd)` after `dispatch_client_fd` returns 1. Add a `test_cluster.c` case: after N
  dispatched connections, the master's open-fd count must not grow, and a `Connection: close` response must
  produce EOF at the client.

### M2 · Large static files are read whole and copied twice; each slow reader pins the full size
- **Impact:** High. Memory DoS. The whole event loop also blocks while a large file is read.
- **Effort:** S (use the existing streaming path)
- **Where:** `lib/static.c:340-371`: files above the 256 KiB cache cap (up to `MAX_STATIC_FILE_SIZE`, 50 MiB)
  are
  1. `malloc(size)` + blocking `fread` of the whole file,
  2. copied again into the arena by `res_send_bytes` (a fallback `malloc` of the same size),
  3. then copied a third time into a connection-owned tail by `flush_connection` on the first `EAGAIN`
     (`lib/connection.c:515`).

  The third copy lives until the client has read everything. S2's write-stall deadline only fires on *zero*
  progress, so a client that reads one byte every 29 s holds it indefinitely.
- **How to reproduce (MEASURED, single process):** add a 40 MiB file under the static root, then:
  - `curl -s -o /dev/null http://127.0.0.1:18090/static/big.bin` → RSS 1.4 MB → **83 MB**.
  - Ten `curl --limit-rate 20k` downloads of the same file → RSS **452 MB**.
  - After the clients disconnect, `leaks` reports 0 leaks and `heap` shows 99 KB in use, yet the footprint stays
    at **441 MB**: `vmmap` shows the freed blocks as `MALLOC_LARGE (empty)`, still counted. The peak therefore
    becomes the process footprint.
- **Fix:** In `static_serve_file`, when `st.st_size > STATIC_CACHE_MAX_ENTRY_BYTES`, call
  `res_send_file(res, content_type, resolved)` instead of reading the file. That path streams through the
  16 KiB `conn->stream_buf` and never touches the arena, so each slow reader costs 16 KiB instead of the file
  size, and nothing blocks. Pair it with P5 (`sendfile`). Optionally add a minimum-rate rule to the S2 write
  deadline (for example, close if fewer than 1 KiB/s over 30 s).

### M3 · No per-worker budget for memory held by in-flight requests and responses
- **Impact:** Medium
- **Effort:** M
- **Where:** `lib/connection.c:721` (`grow_in_buf`, up to ~10 MiB per uploading connection),
  `lib/connection.c:515` (unsent tail copy, up to the full response size), `lib/response.c:381`
  (`res_write` up to `MAX_BODY_SIZE + 8 KiB`).
- **Problem:** Every per-connection cap is about 10 MiB, and `max_connections` defaults to 10,000 per worker.
  Nothing bounds the total, so a worker's memory is limited only by what clients choose to send or refuse to read.
- **Fix:** Add a per-worker counter of bytes held in owned `in_buf`s and owned `out_buf` tails, checked in
  `grow_in_buf` and in the tail copy. Past a budget (`ServerConfig.max_buffered_bytes`, default for example
  256 MiB), reply 503 (uploads) or close the newest stalled connection (writes). Maintaining the counter is one
  add or subtract per allocation.

### M4 · Arena growth always copies (yyjson realloc, `res_write` doubling)
- **Impact:** Low–Medium (large JSON responses, large `res_write` bodies)
- **Effort:** S
- **Where:** `lib/arena.c:62` (`arena_yyjson_realloc` always does `arena_alloc` + `memcpy`),
  `lib/response.c:385-400` (`append_to_out_buf` doubles by allocating a new block).
- **Problem:** When the block being grown is the last allocation in the arena buffer, it can simply be extended.
  Today every doubling leaves a dead copy behind, so a 1 MiB `yyjson_mut_write` or chunked body costs about 2×
  its size, plus a fallback `malloc` for each step past 64 KiB (already noted under "Known gaps" in
  `lib/CLAUDE.md`, without this fix).
- **Fix:** Add `arena_grow(Arena *, void *ptr, size_t old, size_t new)`: if `ptr + align(old) == buf + offset`
  and the new size fits, bump `offset` and return `ptr`; otherwise allocate and copy as now. Use it in both call
  sites. Test: grow the last block in place, grow a block that isn't last (copied), grow past capacity (fallback).

### M5 · Static cache: alias paths create duplicate entries; the cache is per worker
- **Impact:** Low (bounded at 64 MiB per worker)
- **Effort:** S
- **Where:** `lib/static.c:9-67` (keeps `.` segments), `lib/static.c:270` (key = `static_root/subpath`).
- **Problem:** `/static/a.css`, `/static/./a.css` and `/static/././a.css` each create their own entry holding a
  copy of the same bytes. Cycling through 256+ aliases evicts every real entry and forces the slow path
  (`realpath`/`stat`/`fopen`/`fread`) on every request. With N workers the cache can reach N × 64 MiB.
- **Fix:** Drop `.` segments in `static_resolve_relative_path`, as it already rejects `..`. This comes for free
  with the path canonicalization in S1. Mention the per-worker multiplier in `lib/CLAUDE.md`, or lower
  `STATIC_CACHE_MAX_TOTAL_BYTES` when `workers > 1`.

### M6 · A partial request always costs a full 8 KiB owned buffer
- **Impact:** Low
- **Effort:** S
- **Where:** `lib/connection.c:960` (`stop_borrowing_read_buf`: `malloc(BUF_SIZE)` whatever the size of the leftover).
- **Problem:** 10,000 slowloris connections that have each sent 20 bytes hold 80 MiB for up to the 10 s header deadline.
- **Fix:** Allocate `leftover` rounded up to 512 bytes. Growth already happens through `grow_in_buf`/`realloc`
  once the buffer fills; check that the 431 test (`in_len >= in_cap - 1` with no blank line) still uses
  `BUF_SIZE` as its limit, not the smaller capacity.

---

## S — Security

### S1 · Prefix middleware is bypassed with an empty or encoded path segment
- **Impact:** **High**. An auth bypass for the documented pattern: `app_use_prefix(app, "/admin", auth)` (the
  cookbook's own recipe at `lib/examples/cookbook.c:523`, `DOC.md:213`, `README.md:137`), and `router_use`
  middleware on a mounted router (`app_mount` turns it into `app_use_prefix`).
- **Effort:** S
- **Where:** `lib/middleware.c:12-22` (`middleware_prefix_matches`: literal `strncmp` on `req->path`) versus
  `lib/router.c:459-475` (`next_segment` skips empty segments, so `//admin/secret` routes to `/admin/secret`).
  `%2F` is decoded before routing (`lib/http_parser.c:443`), so `/%2Fadmin/secret` behaves the same way.
- **How to reproduce (MEASURED, probe server):**
  ```
  curl -s -w ' [%{http_code}]\n' http://127.0.0.1:18080/admin/secret                  # auth required [401]
  curl -s --path-as-is -w ' [%{http_code}]\n' http://127.0.0.1:18080//admin/secret    # TOP SECRET [200]
  curl -s --path-as-is -w ' [%{http_code}]\n' 'http://127.0.0.1:18080/%2Fadmin/secret' # TOP SECRET [200]
  ```
  As a unit test in `tests/test_cookbook.c`: `fetch("GET //admin/stats HTTP/1.1\r\nHost: x\r\n\r\n")` must not
  return the stats.
- **Related fail-open:** `app_use_prefix(app, "/admin/", mw)` (trailing slash) matches no sub-path at all,
  because the byte after the prefix is never `/` or NUL, so the middleware silently never runs.
  `app_mount` normalizes the trailing slash; `app_use_prefix` and `app_use_body_limit` don't.
- **Fix:**
  1. After decoding in `parse_request_fields`, canonicalize `req->path`:
     - collapse repeated `/`;
     - reject (400) any `.` or `..` segment, or resolve them without going above the root;
     - reject an encoded `%2F` (safest), or keep it encoded so it can't create a segment.
  2. Match every prefix (middleware, body limits, static) on segment boundaries with one shared helper.
  3. Normalize the prefix at registration (strip the trailing `/`).

  Routing, middleware and limits then all see the same path.

### S2 · `app_use_body_limit` is bypassed four ways
- **Impact:** High. The per-route limit is the documented way to keep, for example, a JSON endpoint to 16 KiB.
  Every bypass falls back to the global 10 MiB.
- **Effort:** S
- **Where:** `lib/connection.c:778-782` passes the **raw** request-target (still percent-encoded, query string
  included) to `app_body_limit_for_path` (`lib/router.c:439`). `lib/connection.c:775` skips chunked bodies
  entirely.
- **How to reproduce (MEASURED, probe has `app_use_body_limit(&app, "/upload", 16)`), 5,000-byte body:**
  ```
  POST /upload                               → 413
  POST /upload?x=1                           → 200 "accepted 5000 bytes"   (the '?' fails the boundary check)
  POST //upload                              → 200                          (literal prefix mismatch)
  POST /%75pload                             → 200                          (raw vs decoded path)
  POST /upload  Transfer-Encoding: chunked   → 200                          (chunked is never checked)
  ```
- **Fix:** Compute the limit from the same canonical, decoded, query-free path as S1. For chunked bodies, pass
  the route's limit (not `MAX_BODY_SIZE`) as `max_decoded_len` to `chunked_body_scan_resume` and as the raw cap
  in `grow_in_buf`. That keeps the scan linear and makes the documented "chunked is unaffected" exception
  unnecessary (update `lib/API.md:32`).

### S3 · Chunk-size parsing is lenient (request-smuggling risk behind a proxy)
- **Impact:** Medium. It matters whenever a proxy or load balancer sits in front, which is the recommended TLS
  deployment (`lib/CLAUDE.md`, "No TLS").
- **Effort:** S
- **Where:** `lib/http_parser.c:750` and `:806` use `strtoul(size_buf, ..., 16)`, which accepts leading
  whitespace, a sign, and a `0x` prefix. RFC 9112 allows only `1*HEXDIG`. Trailer lines (`:755-768`) are not
  validated at all, so the bare-LF rule from S11 does not reach them.
- **How to reproduce (MEASURED):** each of these got `HTTP/1.1 200 OK`:
  ```
  printf 'POST /upload HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n0x5\r\nhello\r\n0\r\n\r\n' | nc 127.0.0.1 18080
  (same with "+5" and " 5" as the size line)
  ```
  A front-end that reads `0x5` as `0` (it stops at `x`) considers the body finished. This engine reads
  5 bytes. That disagreement is how request smuggling starts.
- **Fix:** Replace `strtoul` with a hand-written loop: 1–16 hex digits, then optional `BWS ; ext`, nothing else.
  Reject bare LF and control characters in trailer lines. Add the three inputs above to `test_http_hardening.c`
  and the fuzzer seeds.

### S4 · Static mounts serve dotfiles
- **Impact:** Medium. `.env`, `.git/`, `.htpasswd` and editor swap files are served if they exist under the root.
  Deny-by-default (`CLAUDE.md`) suggests these should be 404.
- **Effort:** S
- **Where:** `lib/static.c:45-61` (`static_resolve_relative_path` rejects only `..`).
- **How to reproduce (MEASURED):** `curl http://127.0.0.1:18080/static/.env` → `SECRET_KEY=hunter2 [200]`;
  `/static/.git/config` → `[core] [200]`.
- **Fix:** Return 404 for any segment starting with `.` (the default in Express's `serve-static`). Add
  `app_serve_static_opts` later if someone needs `.well-known`. Also consider sending
  `X-Content-Type-Options: nosniff` on static responses.

### S5 · Multipart: quoted parameters are parsed wrongly and `filename` is returned unsanitized
- **Impact:** Medium. Any app that saves an upload under `part->filename` has a path traversal, and the
  cookbook's upload recipe (`lib/examples/cookbook.c:303-323`) shows `filename` being used.
- **Effort:** S
- **Where:** `lib/multipart.c:17-54` (`extract_param` searches with `strcasestr` and ignores quoting).
- **How to reproduce (MEASURED, a direct call to `parse_multipart_body`):**
  - `name="avatar; filename=../../etc/cron.d/x"` gives
    `name=[avatar; filename=../../etc/cron.d/x]` **and** `filename=[../../etc/cron.d/x"]`: a second parameter
    pulled out of the inside of a quoted value.
  - `filename="../../../root/.ssh/authorized_keys"` is returned exactly as sent.
- **Fix:** Parse `Content-Disposition` as `;`-separated `token=value` pairs, where `value` is a token or a
  quoted string (with `\"` escapes); match the names exactly. Add `multipart_safe_filename(part, out, size)`:
  keep only the basename, drop control characters, reject empty, `.` and `..`, and document it in `lib/API.md`
  as the only name safe for a filesystem.

### S6 · The listening socket always binds every interface (`INADDR_ANY`, IPv4 only)
- **Impact:** Medium. The engine is plaintext by design and expects a TLS proxy in front, but it cannot be
  bound to `127.0.0.1` or a private interface. Anyone who can reach the host can talk to it directly, bypassing
  the proxy's TLS, authentication and rate limits.
- **Effort:** S
- **Where:** `lib/connection.c:77-81` (`create_server_socket`); `ServerConfig` (`lib/app_types.h`) has no address field.
- **Fix:** Add `ServerConfig.bind_address` (default `NULL` = all interfaces, for compatibility; the docs should
  recommend `127.0.0.1` behind a proxy) and use `getaddrinfo(AI_PASSIVE)` so IPv6 works too.
  `cluster_listen`'s preflight and the macOS single acceptor both go through `create_server_socket`, so one
  change covers every path.

### S7 · A missing or duplicated `Host` header is accepted
- **Impact:** Low–Medium. RFC 9112 §3.2 says a server MUST answer 400 for an HTTP/1.1 request with no `Host`
  or with more than one. Accepting them leaves room for host-header confusion behind a proxy (for example,
  virtual-host routing or cache keys computed from a different `Host` than the app sees).
- **Effort:** S
- **Where:** `lib/http_parser.c:412-478` (`parse_request_fields`).
- **How to reproduce (MEASURED):** `GET /static/a.txt HTTP/1.1` with no `Host` → 200; with `Host: a` and
  `Host: b` → 200.
- **Fix:** In `parse_request_fields`, count `Host` headers while copying the views. For `minor_version >= 1`,
  return -1 (400) when the count is not exactly 1.

### S8 · Form fields: `%00` is not rejected, and values are cut at 255 without notice
- **Impact:** Low–Medium. It's the same bypass S6 closed for the path and query, in form bodies:
  `file=shell.php%00.png` reads as `shell.php`.
- **Effort:** S
- **Where:** `lib/urlencoded.c:37-38` ignores `url_decode`'s `-1`. `lib/urlencoded.c:33` truncates values to 255.
- **How to reproduce (MEASURED):** `curl -d 'file=shell.php%00.png' http://127.0.0.1:18080/form` → `file=[shell.php]`.
- **Fix:** Have `parse_urlencoded_body` return `int`: -1 on an embedded NUL, -2 on a value too long for its
  slot, so the handler can answer 400 instead of acting on a shortened value. (This is an API change; update
  `lib/API.md` and the cookbook.)

### S9 · Response header values are silently truncated at 255 characters
- **Impact:** Low–Medium. A `Content-Security-Policy` longer than 255 characters (common) is cut mid-directive,
  which weakens or breaks the policy. A redirect `Location` longer than 255 characters points somewhere else.
- **Effort:** S (reject) or M (store in the arena)
- **Where:** `lib/response.c:72` and `:86` (`strncpy` into `ResponseHeader.value[256]`); `res_redirect`
  (`:278-291`) goes through the same path.
- **How to reproduce (MEASURED):** set a 399-character CSP; the header line received is 281 characters
  (name + 255).
- **Fix:** At minimum, drop the header with a `stderr` message (as `res_set_cookie` already does for
  over-length cookies) and have `res_redirect` answer 500, never send a shortened value. Better: copy header
  values into the request arena (pointer + length) and remove the fixed 255-character cap.

### S10 · `arena_alloc` has no size-overflow check
- **Impact:** Low (hardening; no current caller passes a size near `SIZE_MAX`)
- **Effort:** S
- **Where:** `lib/arena.c:19` (`align_up(size)` wraps around for sizes near `SIZE_MAX`), `:22`
  (`offset + aligned_size` can overflow), `:29` (`sizeof(ArenaNode) + size` can overflow).
- **Fix:** Return `NULL` if `size > SIZE_MAX - ALIGNMENT - sizeof(ArenaNode)` and check with
  `aligned_size > a->cap - a->offset` instead of adding. It costs one comparison, and the arena is on every
  request path, which yyjson and application code also reach.

---

## Appendix A: probe server used for the reproductions

Build it with gcc-16 against `build/lib/libcexpress.a` (`make all` first). Create `public/` with `a.txt`, a
40 MiB `big.bin`, `.env` and `.git/config`, then run it (`PORT=18080 ./probe`, or add `WORKERS=2` for M1).

```c
#include <stdio.h>
#include <stdlib.h>
#include "cexpress.h"

static void deny_admin(const Request *req, Response *res, MiddlewareChain *chain) {
    (void)req; (void)chain;
    res_status(res, 401); res_send(res, "auth required");      /* never calls chain_next */
}
static void secret(const Request *req, Response *res) { (void)req; res_send(res, "TOP SECRET\n"); }
static void upload(const Request *req, Response *res) {
    char b[64]; snprintf(b, sizeof b, "accepted %d bytes\n", req->content_length); res_send(res, b);
}
static void echo_form(const Request *req, Response *res) {
    UrlEncodedForm f; parse_urlencoded_body(req->body, (size_t)req->content_length, &f);
    const char *v = urlencoded_get_field(&f, "file");
    char b[300]; snprintf(b, sizeof b, "file=[%s]\n", v ? v : "(null)"); res_send(res, b);
}
static void csp(const Request *req, Response *res) {
    (void)req;
    char v[400]; for (int i = 0; i < 399; i++) v[i] = 'a' + i % 26; v[399] = 0;
    memcpy(v, "default-src 'self'; ", 20);
    res_set_header(res, "Content-Security-Policy", v); res_send(res, "ok\n");
}
int main(void) {
    App app; app_init(&app);
    const char *w = getenv("WORKERS"); app.config.workers = w ? atoi(w) : 1;
    app_use_prefix(&app, "/admin", deny_admin);
    app_get(&app, "/admin/secret", secret);
    app_use_body_limit(&app, "/upload", 16);
    app_post(&app, "/upload", upload);
    app_post(&app, "/form", echo_form);
    app_get(&app, "/csp", csp);
    app_serve_static(&app, "/static", "public");
    const char *p = getenv("PORT");
    app_listen(&app, p ? atoi(p) : 18080);
    app_destroy(&app);
    return 0;
}
```

P3's timing loop (`quad.c`): build an 8 KiB head of `X-Hn: 000…` lines with no blank line, then time
`for (len = 1; len <= n; len++) parse_request_head(buf, len, &head);` against the same loop stepping 1,460
bytes at a time.

P1's syscall count: build the probe on Alpine with `-DCEXPRESS_USE_EPOLL` and the Linux sources, run it under
`strace -f -c` (`docker run --cap-add=SYS_PTRACE --security-opt seccomp=unconfined`), and send 1,000 keep-alive
requests with a single `curl` invocation.

## Appendix B: checked and found fine

- Path traversal in static mounts: `..` rejected, symlink escape caught by `realpath` + containment check.
- Response splitting: control characters refused in header names/values, cookies, trailers and redirects.
- `Content-Length` parsing: digits only, duplicates must agree, CL together with chunked rejected, overflow capped.
- Router backtracking: bounded by route-tree depth (developer-controlled), no attacker-driven blow-up.
- The shared arena and `read_buf` (M1/M2): lifetimes hold across pipelining, streaming and early closes. The
  `EAGAIN` tail copy is what makes this safe (and is also the cost M2 and P4 point at).
