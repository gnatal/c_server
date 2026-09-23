# Improvements: what to fix to make CExpress faster, safer and lighter

Companion to [`finds.md`](finds.md) (which items 1–9 below build on) and [`lib/CLAUDE.md`](lib/CLAUDE.md) ("Known gaps"). Every entry has the same three parts you asked for: **the problem in the code**, **how to fix it**, and **the probable gain**.

## How to read the gains

Every gain carries one of three tags, so you can tell a number from a guess:

| Tag | Meaning |
|---|---|
| **MEASURED** | I ran an experiment on this machine (Apple M3 Pro, macOS, gcc-16 -O2) and the number is from that run. Single runs unless noted; `wrk` shares the machine with the server. |
| **PROJECTED** | Arithmetic on measured components (for example "parse is 3 passes of ~150 ns, so removing 2 saves ~300 ns"). Sound, but the end-to-end effect was not run. |
| **ESTIMATED** | Reasoning from how the kernel or library works, or published behavior of similar systems. Not measured here; treat as a hypothesis to test. |

Nothing was measured on Linux, so everything about io_uring and epoll is ESTIMATED, **except C6** (added 2026-09-22, MEASURED on real Linux via `docker_stress_test.sh` - see that entry for how). Experiments were done in scratch copies of the code; **no repository file was changed for this document** (the experiment programs live outside the repo; say so if you want them added under `tests/experiments/`).

Effort: **S** = under a day, **M** = a few days, **L** = a week or more. Severity is for security/reliability items only.

---

## 1. Summary

| ID | Problem | Kind | Sev. | Effort | Probable gain | Evidence |
|---|---|---|---|---|---|---|
| S1 | ~~Slow-drip clients hold connections forever~~ | Security | High | S | **FIXED 2026-09-22**: see `improvements_progress.md` | MEASURED (168 s and still open) |
| S2 | ~~No write timeout: a client that stops reading holds its connection forever~~ | Security | High | S | **FIXED 2026-09-22**: see `improvements_progress.md` | Code reading |
| S3 | ~~No connection limit or overload handling~~ | Security | High | S | **FIXED 2026-09-22**: see `improvements_progress.md` | MEASURED (fd limit) |
| S4 | ~~A declared `Content-Length` reserves 10 MiB per connection at once~~ | Security / Memory | Med | S | **FIXED 2026-09-22**: see `improvements_progress.md` | MEASURED (+3,000 MB virtual for 300 conns) |
| S5 | ~~Header values over 255 chars are silently truncated (JWTs break)~~ | Security / Correctness | Med | M | **FIXED 2026-09-22**: see `improvements_progress.md` | MEASURED |
| S6 | ~~`%00` in a path truncates it~~ | Security | Med | S | **FIXED 2026-09-22**: see `improvements_progress.md` | MEASURED (`style.css%00.png` → 200) |
| S7 | ~~Failing workers are respawned with no backoff~~ | Reliability | High | S | **FIXED 2026-09-22**: see `improvements_progress.md` | MEASURED (10,594 respawns in 4 s) |
| S8 | ~~Malformed request line gets no response~~ | Security / Correctness | Med | S | **FIXED 2026-09-23**: see `improvements_progress.md` | MEASURED |
| S10 | ~~TLS hardening gaps (renegotiation, handshake deadline)~~ | Security | Low | S | **REMOVED 2026-09-22**: TLS was removed from the engine, see `improvements_progress.md` | ESTIMATED |
| S11 | ~~Bare `\n` and substring `chunked` accepted (smuggling ambiguity)~~ | Security | Low | S | **FIXED 2026-09-23**: see `improvements_progress.md` | Code reading + MEASURED (bare LF) |
| S12 | ~~Startup allocations and `exit()` calls in library code~~ | Reliability | Low | S | **FIXED 2026-09-23**: see `improvements_progress.md` | Code reading |
| P1 | ~~Static and file responses go through slow paths~~ (static-file cache only) | Performance | | M | **PARTIALLY FIXED 2026-09-22**: see `improvements_progress.md` - the `/static` mount's 6.6× gap is closed; `res_send_file`/large-file streaming (item 1's other sub-parts) are untouched | MEASURED |
| P2 | ~~Request headers are parsed three times~~ | Performance | | M | **FIXED 2026-09-22**: see `improvements_progress.md` - `last_len` incremental resume across separate `recv`s (and P9, which depends on it) are untouched | MEASURED (was PROJECTED from MEASURED parts) |
| P3 | ~~Headers are copied into fixed 19 KB `Request` arrays~~ | Performance / Memory | | L | **FIXED 2026-09-22**: see `improvements_progress.md` - headers are views (S5's `-3`/431 retired) and cookies parse lazily; query parsing left eager, out of scope | MEASURED (was PROJECTED) |
| P4 | epoll ~~and io_uring~~ issue a syscall on every interest change | Performance | | S | ~5–25% per keep-alive request on Linux | ESTIMATED. **io_uring half done 2026-09-23 with C6** (no-op changes skipped, SQEs batched); epoll half open |
| P5 | ~~With TLS on, every loop iteration scans the whole connection table~~ | Performance | | S | **REMOVED 2026-09-22**: TLS was removed from the engine, see `improvements_progress.md` | MEASURED |
| P7 | ~~Router child lookup is a linear scan~~ | Performance | | S | **FIXED 2026-09-22**: see `improvements_progress.md` | MEASURED (problem and fix) |
| P8 | ~~Chunked bodies are re-scanned from the start on every `recv`~~ | Performance / Security | Med | M | **FIXED 2026-09-23**: see `improvements_progress.md` - MEASURED 10 MiB worst case in 14 ms (was ~71 s extrapolated) | MEASURED |
| P9 | ~~HTTP pipelining is dropped~~ | Performance / Correctness | | M | **FIXED 2026-09-23**: see `improvements_progress.md` - MEASURED 16/16 pipelined requests answered (was 1/16); depth-16 `/ping` at 1.7x the non-pipelined rate, non-pipelined unchanged | MEASURED |
| P10 | ~~`accept` path uses 4 syscalls plus setup~~ | Performance | | S | **FIXED 2026-09-23**: see `improvements_progress.md` - accept is 1 syscall on Linux and macOS; MEASURED on Linux 10.0 → 7.0 syscalls per `Connection: close` request | MEASURED |
| M1 | ~~64 KiB arena per connection~~ | Memory | | M | **FIXED 2026-09-22**: see `improvements_progress.md` - MEASURED 8.2 KB per idle connection, matching the projected 8.4 KB | MEASURED |
| M2 | ~~8 KiB input buffer per idle connection~~ | Memory | | M | **FIXED 2026-09-23**: see `improvements_progress.md` - MEASURED 8,457 → 239 B per idle keep-alive connection (5,000 connections: 41 MB → 1.2 MB), throughput unchanged | MEASURED |
| M3 | ~~TLS connections keep large SSL buffers~~ | Memory | | S | **REMOVED 2026-09-22**: TLS was removed from the engine, see `improvements_progress.md` | MEASURED |
| M4 | ~~Request bodies are copied twice~~ | Memory | | M | **FIXED 2026-09-23**: see `improvements_progress.md` - body is a view into the input buffer (chunked decoded in place); MEASURED peak RSS for a 9.5 MiB upload 41.7 → 32.1 MB (−23%, not the projected −50%: `realloc` growth dominates what's left) | MEASURED |
| M5 | ~~"Streaming" responses are fully buffered~~ | Memory / Feature | | L | **FIXED 2026-09-23**: see `improvements_progress.md` | MEASURED (38 MB → 1.6 MB RSS for an 8.4 MB body) |
| M6 | ~~`Route` embeds a `PATH_MAX` buffer~~ | Memory | | S | **FIXED 2026-09-23**: see `improvements_progress.md` | MEASURED (`sizeof(Route)` 1,368 → 352 B on macOS; 4,440 → 352 B on Linux) |
| C1 | ~~`Expect: 100-continue` is ignored~~ | Correctness | | S | **FIXED 2026-09-23**: see `improvements_progress.md` | MEASURED (2 MiB curl upload 1.007 s → 1–2 ms, Content-Length and chunked) |
| C2 | ~~Path parameter names are stored per tree position~~ | Correctness | | S | **FIXED 2026-09-23**: see `improvements_progress.md` | MEASURED |
| C3 | No `Date` header; 204 carries `Content-Length` | Correctness | | S | RFC compliance | MEASURED |
| C4 | ~~macOS `SO_REUSEPORT` does not balance workers~~ | Reliability | | M | **FIXED 2026-09-22**: see `improvements_progress.md` | MEASURED (imbalance, and the fix) |
| C5 | io_uring failure kills the server (no epoll fallback) | Reliability | | M | Runs under restrictive seccomp | Code reading |
| C6 | ~~io_uring backend closes every keep-alive connection after its first request~~ | Correctness / Reliability | | S | **FIXED 2026-09-23**: see `improvements_progress.md` | MEASURED (real Linux, via Docker: 1/20 → 1000/1000 requests per connection) |
| T1 | Test and tooling gaps that let the bugs above through | Testing | | S–M | Prevents regressions | Code reading |

---

## 2. Suggested order

**Quick wins (each S, do first):** S6, S7, S1, S2, C1, C3, P5, M3, P7, **C6 (MEASURED 2026-09-22, breaks Linux keep-alive - do this one early)**.
**Then (M):** S3+S4 together, P1, M1+M2 together, P4, P2, C2, P8, P9.
**Larger (L):** P3, M5, C4, C5.

Dependencies to respect: M1 and M2 change how `Connection.arena` and `in_buf` are owned (do them together); P9 (pipelining) needs P2's cached `header_len`; S5 is solved properly only by P3; S1 and S2 share the same per-connection deadline fields.

---

## 3. Security and robustness

### S1 · ~~Slow-drip clients hold connections forever~~ (FIXED 2026-09-22)
**Fixed on 2026-09-22** — see `improvements_progress.md` for the fix record. Kept below for historical
record.

**Problem.** `last_activity` is refreshed on **every received byte** (`lib/connection.c:437`, `lib/tls.c:170`) and `close_idle_connections` (`connection.c:483-509`) only closes a connection that has been silent for a full 60 s. A client that sends one byte every 59 s is never "idle". There is no deadline for receiving the request line and headers, and none for the body, so the documented Slowloris defence (431) only covers *size*, not *time*. A TLS client that connects and never finishes the handshake is bound by the same rule.
**Measured.** A client sending 1 byte every 20 s was still connected after **168 s** (the test was stopped, not the connection). Each held connection pins a file descriptor and ~25 KB resident (72 KB allocated).
**Fix.** Add `request_started` to `Connection`: set when the first byte of a request arrives after an idle period, cleared when a response is queued. In `close_idle_connections` close (408) any connection whose `now - request_started` exceeds a header deadline (10–30 s) or, once headers are complete, a body deadline (a total limit, or a minimum-rate rule such as 1 KB/s after a grace period). Apply the same clock to unfinished TLS handshakes. One store per request; no hot-path cost.
**Probable gain.** Closes an unauthenticated resource-exhaustion attack. Cost: one timestamp write per request (well under 1 ns amortized).

### S2 · ~~A client that stops reading holds its connection forever~~ (FIXED 2026-09-22)
**Fixed on 2026-09-22** — see `improvements_progress.md` for the fix record. Kept below for historical
record.

**Problem.** `close_idle_connections` skips any connection with `out_buf != NULL` or `file_fd >= 0` (`connection.c:495-497`, commented "slow readers are not this timeout's job"). A client that requests a large response (a big static file, a 10 MiB streamed body) and never reads it keeps the fd, the arena and the whole response buffer indefinitely.
**Fix.** Track `last_write_progress` (advance when `write` accepts bytes, which `flush_connection` already sees at `:303`) and close when a pending response makes no progress for N seconds (30–60 s). Optionally cap pending output per connection.
**Probable gain.** Closes a second DoS with the same mechanism as S1; up to `MAX_BODY_SIZE` (10 MiB) of memory per stuck connection returned to the pool. Not tested (needs a response larger than the socket buffers).

### S3 · ~~No connection limit, no overload behavior~~ (FIXED 2026-09-22)
**Fixed on 2026-09-22** — see `improvements_progress.md` for the fix record (note: the fd-exhaustion/
EMFILE half carries a caveat, not a full fix, per that record). Kept below for historical record.

**Problem.** Connections are bounded only by `RLIMIT_NOFILE` (`accept_connections`, `connection.c:241-285`); there is no per-worker, per-IP or global limit and no reserve for overload. At the descriptor limit `accept()` fails and the loop just stops accepting.
**Measured.** With `ulimit -n 40` and 100 clients, the first clients were served and a late client received an empty reply; the server stayed idle (0% CPU, so no busy loop on macOS) and never told anyone it was full. Memory scales as ~25 KB per connection (macOS), so 100,000 connections is ~2.5 GB with no cap.
**Fix.** (1) Config `max_connections` per worker; beyond it, accept and immediately send `503 Service Unavailable` + `Connection: close` (or stop watching the listen socket until below the limit). (2) Keep a spare descriptor open so that on `EMFILE` you can accept, answer 503 and close. (3) Optional per-IP cap. Also raise `BACKLOG` (128, `app_types.h`) toward `SOMAXCONN`; on Linux the kernel cap is usually larger (macOS `kern.ipc.somaxconn` is 128 here, so it cannot help there).
**Probable gain.** Bounded memory under attack; graceful degradation instead of silent drops. No cost when below the limit.

### S4 · ~~A declared `Content-Length` reserves 10 MiB per connection immediately~~ (FIXED 2026-09-22)
**Fixed on 2026-09-22** — see `improvements_progress.md` for the fix record (note: per-route body
limits cover `Content-Length` only, not chunked bodies, per that record). Kept below for historical
record.

**Problem.** As soon as the 8 KiB buffer fills with a body pending, `grow_in_buf` (`connection.c:398-418`) reallocates straight to `header_len + content_length + 1`, trusting the client's header. `MAX_BODY_SIZE` is also a single global 10 MiB for every route.
**Measured.** 300 connections, each declaring a 10 MiB body and sending 9 KB: virtual size rose by **~3,000 MB**, resident memory by only ~14 MB. So this is a reservation, not (yet) resident memory, but it can hit strict-overcommit systems, `RLIMIT_AS`, or container accounting that counts mapped memory, and it hands an attacker a 1000:1 reservation ratio.
**Fix.** Grow geometrically as bytes actually arrive (as the chunked path already does), and add per-route or per-prefix body limits (`app_use_body_limit(prefix, bytes)`, default e.g. 1 MiB, with 10 MiB opt-in). Reject early with 413 when the declared length exceeds the route limit.
**Probable gain.** Virtual reservation proportional to data received instead of declared; a JSON API rejects a 5 MiB body at the header stage instead of buffering it.

### S5 · ~~Header values over 255 characters are silently truncated~~ (FIXED 2026-09-22)
**Fixed on 2026-09-22** — see `improvements_progress.md` for the fix record (note: individual
post-split cookie values and path/query param values remain truncating, per that record). **Superseded
the same day by P3**, below: once headers became views into the input buffer instead of fixed-size
copies, the raise-the-cap-and-431 fix here was replaced by "no cap at all" (P3's own record has the
retirement of the `-3`/431 return code this fix introduced). Kept below for historical record.

**Problem.** `copy_bounded` (`http_parser.c:22`) cuts header values at 255 bytes. Bearer tokens (JWTs are commonly 300–1,000 characters) and long cookies are silently shortened, so a valid `Authorization: Bearer <JWT>` fails comparison, and code that only checks a prefix would accept a wrong token. No error is raised.
**Measured.** A 400-character header value was stored as 255 characters with a successful parse.
**Fix.** Short term: answer `431` when a header exceeds the stored size, or raise the value size for `Authorization` and `Cookie`. Proper fix: P3 (views into the input buffer, no per-header cap).
**Probable gain.** Removes a silent failure class for token-based auth; correctness, no performance cost.

### S6 · ~~`%00` in a path truncates it~~ (FIXED 2026-09-22)
**Fixed on 2026-09-22** — see `improvements_progress.md` for the fix record. Kept below for historical
record.

**Problem.** `decode_bounded` (`http_parser.c:32-49`) writes a decoded `%00` as a real NUL, and everything after it is treated as a C string terminator.
**Measured.** `GET /static/style.css%00.png` returned **200** with `style.css`: the router and the static handler saw `/static/style.css`. Any application that checks a suffix or extension on `req->path` before using it can be bypassed the same way.
**Fix.** After decoding the path (and query names/values, cookie values), reject an embedded NUL with `400`. It is one `memchr` over data already in cache.
**Probable gain.** Removes a well-known bypass primitive for a few nanoseconds per request.

### S7 · ~~A failing worker is respawned instantly, forever~~ (FIXED 2026-09-22)
**Fixed on 2026-09-22** — see `improvements_progress.md` for the fix record. Kept below for historical
record.

**Problem.** `cluster_listen` (`lib/cluster.c:158-166`) respawns any worker that exits abnormally (non-zero code or signal) with no delay or limit. `create_server_socket` calls `exit(EXIT_FAILURE)` on a bind error (`connection.c:74-76`), and an `event_loop_init` failure exits the worker (`connection.c:548-551`).
**Measured.** With the port already taken, `WORKERS=2` produced **10,594 respawn messages and 31,788 log lines in about 4 seconds** (~2,600 forks per second).
**Fix.** Exponential backoff per slot (for example 100 ms doubling to 30 s) plus a restart budget (N failures in M seconds); when exhausted, stop and exit the master with a non-zero status. Better: have the master create/verify the listen socket (or run a startup handshake through a pipe) before forking, so a fatal configuration error stops the whole server once. Make `create_server_socket` return an error instead of calling `exit()` (S12).
**Probable gain.** Turns a CPU-and-log storm into one clear error message; protects orchestrators from crash-loop pressure.

### S8 · ~~A malformed request line gets no response~~ (FIXED 2026-09-23)
**Fixed on 2026-09-23** — see `improvements_progress.md` for the fix record. Kept below for historical
record.

**Problem.** (Finding 1 in `finds.md`.) `request_framing` returns `-1` with `header_len == 0` when picohttpparser rejects the request; `request_is_complete` (`http_parser.c:224-237`) tests `header_len == 0` first and reports "need more bytes".
**Measured.** `GET /\r\n\r\n`, `GET / HTTP/2.0` and plain garbage all got no response and the connection stayed open.
**Fix.** In `request_is_complete`, check `content_length < 0` before `header_len == 0` (two lines) so the parser reports 400. Add the regression to `tests/test_connection.c`.
**Probable gain.** Correct 400s, and malformed connections are freed immediately instead of after 8 KiB or 60 s+ (and, until S1 is fixed, potentially never). No hot-path cost.

### S10 · ~~TLS hardening gaps~~ (REMOVED 2026-09-22)
**TLS was removed from the engine entirely on 2026-09-22** — see `improvements_progress.md` for the removal record
and `lib/CLAUDE.md`'s "No TLS" note for the rationale (TLS termination belongs at a gateway/reverse proxy in front
of an HTTP/1.1 parsing library, not inside it). This entry is kept for historical record; it no longer applies.

**Problem.** `tls_init_app` (`lib/tls.c:43-59`) sets a TLS 1.2 floor and server cipher preference, which is good, but does not set `SSL_OP_NO_RENEGOTIATION` (client-initiated renegotiation on TLS 1.2 is a known CPU-exhaustion vector), has no handshake deadline (S1), and leaves session-ticket and cipher-list policy at library defaults.
**Fix.** Add `SSL_OP_NO_RENEGOTIATION`, decide ticket policy (`SSL_OP_NO_TICKET` or rotate keys across workers, since each worker has its own context), and expose an optional cipher/curve configuration.
**Probable gain.** Hardening; not exercised here (ESTIMATED). Also note that with several workers each has an independent session cache, so resumption only works if tickets share a key.

### S11 · ~~Parser accepts ambiguous framing that proxies may read differently~~ (FIXED 2026-09-23)
**Fixed on 2026-09-23** — see `improvements_progress.md` for the fix record. Kept below for historical
record.

**Problem.** picohttpparser accepts bare `\n` line endings (MEASURED: a request with only `\n` parsed with its header). `request_framing` and `scan_framing` decide "chunked" by searching the `Transfer-Encoding` value for the substring `chunked` (`http_parser.c:126-131`), so `Transfer-Encoding: xchunked` counts. A front proxy that reads either differently can desynchronize request boundaries.
**Fix.** Reject request lines and headers that end in a bare `\n` (400); accept `chunked` only as the final comma-separated token of `Transfer-Encoding`, reject any other `Transfer-Encoding` on requests with 501/400.
**Probable gain.** Closes desync ambiguity when deployed behind a proxy; negligible cost. Not exploited here (Low).

### S12 · ~~Startup allocations unchecked; library calls `exit()`~~ (FIXED 2026-09-23)
**Fixed on 2026-09-23** — see `improvements_progress.md` for the fix record. Kept below for historical
record.

**Problem.** `app_add_route_mw` (`router.c:102`) does `malloc(sizeof(Route))` and writes to it with no check; `create_patricia_node` (`:404`) and the `realloc` of `children` (`:453`) are unchecked; `fill_route` truncates a route pattern over 255 characters with `strncpy` (`:47`), silently registering a *different* route. `create_server_socket` calls `exit()`/`perror` on failure.
**Fix.** Check every allocation and return an error; reject (not truncate) over-long patterns with a message; return error codes from `create_server_socket` and `event_loop_init` so the application decides.
**Probable gain.** Robustness at startup and better embedding behavior; no runtime cost.

---

## 4. Performance

### P1 · ~~Static and file responses go through slow paths~~ (PARTIALLY FIXED 2026-09-22)
**The `/static` mount half of this was fixed on 2026-09-22** — see `improvements_progress.md` for the fix
record (a small in-memory file cache in `static.c`, fix (1) below). `res_send_file`'s separate head/body
writes, `sendfile(2)` for large files, `realpath` caching for the resolve step, and `ETag`/`Last-Modified`/
`304`/`Cache-Control` (fixes (2)-(5) below) are untouched — still open. Kept below for historical record.

**Problem.** Three separate paths, all slow:
- `res_send_file` (`response.c:502`): per request `open`, `fstat`, write the head alone, `read` into an arena chunk, `write` the body, `close` (about 7 syscalls in all). The head and the body are two separate `write` calls on a `TCP_NODELAY` socket, so they will normally leave as two TCP segments (inferred from the code; packets were not captured).
- `app_serve_static` / `static_serve_file` (`static.c:138-229`): `realpath` (twice for a directory index) plus `stat` per request, then `fopen`/`malloc`/`fread` of the **whole file** (up to 50 MiB) on the event loop, then a second copy into the arena. A 50 MiB file blocks every connection on that worker while it is read, and holds ~100 MiB transiently.
- No `ETag`/`Last-Modified`/`304`, no `Cache-Control`, no `Range`.
**Measured** (4 workers, 100 connections, the same 6,481-byte page):

| Path | Requests/sec |
|---|---|
| Served from a memory buffer with `res_send_bytes` | 249,811 and 256,126 (two rounds) |
| `res_send_file` (what `GET /` does) | 66,600 and 65,820 |
| `/static/index.html` (the `app_serve_static` mount) | 36,010 |
| `/static/style.css` (a 52-byte file) | 37,647 |

**Fix.** (1) A small-file cache: on first request, read files up to a size cap (say 256 KB each, 64 MB total) into memory with their mtime; revalidate with `stat` at most once per second per file; serve with `res_send_bytes`. (2) Build head and body into one buffer (or use `writev`) so one segment leaves. (3) For large files use `sendfile(2)` (plain sockets) instead of `read`+`write`, and stream instead of reading the whole file in `static_serve_file`. (4) Resolve and cache the `realpath` result per file. (5) Emit `ETag`/`Last-Modified` and answer `If-None-Match`/`If-Modified-Since` with 304.
**Probable gain.** **MEASURED 3.8×** for a small file (66k → 250k req/s); the `/static` mount is 6.6× below the in-memory path (37.6k vs 250k), so a cache there is PROJECTED at up to ~6× for small files. For large files the gain is memory (50 MiB → 16 KiB per request) and no event-loop stalls (ESTIMATED). 304 responses also cut bytes on the wire.

### P2 · ~~Request headers are parsed three times~~ (FIXED 2026-09-22)
**Fixed on 2026-09-22** — see `improvements_progress.md` for the fix record (note: `last_len` incremental
resume across separate `recv`s, and P9 which depends on it, are untouched, per that record). Kept below
for historical record.

**Problem.** For every request `handle_readable` calls `request_is_complete`, which runs `request_framing` (a full `phr_parse_request`, `http_parser.c:80`); then `parse_http_request` runs `phr_parse_request` again (`:259`) and then `request_framing` again (`:296`). On a request that arrives in several `recv`s the first pass repeats each time. picohttpparser has an incremental mode (`last_len`) that is never used (always `0`).
**Measured** (`make bench`-style harness, single core):

| Request | `request_is_complete` | `parse_http_request` (contains 2 more passes) | one bare `phr_parse_request` | whole pure path |
|---|---|---|---|---|
| minimal GET | 21 ns | 107–111 ns | 18–20 ns | 208 ns |
| browser-shaped GET | 165–167 ns | 538–546 ns | 149–150 ns | 781 ns |

**Fix.** Parse once. Have the first pass fill a small `ParsedHead { header_len, content_length, chunked, phr_headers[], num_headers }` stored on the `Connection`, pass `last_len` for incremental resume, and let `parse_http_request` consume that result instead of re-tokenizing.
**Probable gain.** **PROJECTED** ≈ 300 ns of the 781 ns pure path for a browser-shaped GET (−38%) and ≈ 40 ns of 208 ns for a minimal GET (−20%). End to end the pure path is roughly 1–5% of a `/ping` request (syscalls dominate), so expect low single-digit percent for tiny requests and up to ~10% for header-heavy ones.

### P3 · ~~Headers are copied into fixed arrays inside a 19 KB `Request`~~ (FIXED 2026-09-22)
**Fixed on 2026-09-22** — see `improvements_progress.md` for the fix record (note: query-string parsing
stayed eager, out of scope, per that record). Kept below for historical record.

**Problem.** `parse_http_request` copies every header name/value, the cookies and query pairs into fixed arrays (`Request` is 19,000 bytes; `http_parser.c:282-289`, `parse_cookies`, `parse_query_string`), decoding query strings eagerly whether or not the handler reads them. This is also what causes the 255-character truncation (S5) and the 32-header cap.
**Measured.** About 240 ns of the 538 ns in `parse_http_request` for the browser-shaped request is copying and decoding beyond the two tokenizer passes.
**Fix.** Keep `struct phr_header`-style views (`ptr`, `len`) into `in_buf` and materialize NUL-terminated copies lazily in the arena on first `req_get_header` / `req_get_query`. Parse cookies and query strings on first access. `Request` shrinks to ~1–2 KB.
**Probable gain.** **PROJECTED** a further ~200 ns on browser-shaped requests (P2 + P3 together: ~500 ns of 781 ns, −64%; ~80 of 208 ns minimal, −38%), smaller stack and cache footprint, and S5 disappears. API change: pointers from `req_get_*` stay valid until the handler returns (as today).

### P4 · epoll and io_uring issue a syscall on every interest change
**io_uring half done 2026-09-23 as part of C6** (see `improvements_progress.md`): an unchanged interest is skipped and
SQEs are submitted once per loop turn. The epoll half below is still open.

**Problem.** Only the kqueue backend uses `events_watched` to skip redundant changes. `flush_connection` calls `event_loop_unwatch_write` after **every** keep-alive response (`connection.c:348`), even though write interest was never registered. On epoll that is an `epoll_ctl(MOD)` per response (`event_loop_epoll.c:255-282`); on io_uring it is a poll-remove plus a new multishot poll plus an `io_uring_submit` (`event_loop_io_uring.c:110-139`, `181-193`).
**Fix.** Add the same early-out as kqueue: in `unwatch_write`, return immediately if `!(events_watched & EVENT_WRITE)`; in `watch_read`, if already watching. For io_uring, prefer `IORING_POLL_UPDATE_EVENTS` (or leave the read poll armed and add write polls only when needed) over remove+add.
**Probable gain.** **ESTIMATED** 5–15% per keep-alive request on epoll (one of three syscalls removed) and 10–25% on io_uring (two SQEs plus a submit removed); to be confirmed with a Linux benchmark (not run here).

### P5 · ~~With TLS on, every loop iteration scans the whole connection table~~ (REMOVED 2026-09-22)
**TLS was removed from the engine entirely on 2026-09-22** — see `improvements_progress.md`. Removing TLS also
removed the `tls_has_pending` scan this entry is about, as a side effect rather than a targeted fix: there is no
longer a hidden per-connection buffer (OpenSSL's) that the event loop needs to poll separately from the socket.
Kept for historical record.

**Problem.** After each `event_loop_poll` batch, `app_listen_worker` loops over `fd = 0 … connections_cap` checking `tls_has_pending` (`connection.c:643-651`). Cost is proportional to the table size (which only grows) and each non-NULL slot is dereferenced, so it costs cache misses per idle connection, per batch.
**Measured** (1 worker, TLS, `wrk -c50` on `/ping`):

| Idle TLS connections held open | Requests/sec |
|---|---|
| 0 | 224,555 |
| 3,000 (baseline code) | 180,478 / 181,332 |
| 3,000, scan only the connections that had an event in this batch | **226,174** |

**Fix.** Iterate only the events just processed (or keep a short "has pending" list appended to by `tls_connection_read`). About six lines.
**Probable gain.** **MEASURED +25%** at 3,000 idle connections (back to the no-idle-connections baseline); the loss grows with connection count, so the win is larger at 10k+.

### P7 · ~~Router child lookup is a linear scan~~ (FIXED 2026-09-22)
**Fixed on 2026-09-22** — see `improvements_progress.md` for the fix record. Kept below for historical
record.

**Problem.** Each Patricia node keeps `children` in an unsorted array and finds a segment with a `memcmp` loop (`router.c:443-449` on insert, `:491-500` on lookup). A path segment with many siblings (`/api/<many resources>`) is O(siblings).
**Measured** (lookup of the last-registered literal route among N siblings):

| Sibling routes | Lookup |
|---|---|
| 10 | 61 ns |
| 100 | 266 ns |
| 1,000 | 2,369 ns |
| 5,000 | 8,401 ns |

**Fix.** Keep children sorted by segment and binary-search, or hash small segments (or index by first byte). Static routes rarely exceed dozens of siblings, so this matters only for large route tables; parameterized routes are unaffected.
**Probable gain.** **PROJECTED** ~0.1 µs regardless of fan-out (≈ 80× at 5,000 siblings; ≈ 2–3× at 100). Irrelevant for a 20-route app; worth it if routes are generated.

### P8 · ~~Chunked bodies are re-scanned from the start on every `recv`~~ (FIXED 2026-09-23)
**Fixed on 2026-09-23** — see `improvements_progress.md` for the fix record. Kept below for historical record.

**Problem.** `request_is_complete` → `chunked_body_scan` restarts at byte 0 of the body each time new data arrives (`http_parser.c:232-234`, `:451-514`). Cost is quadratic in the number of `recv`s.
**Measured.** `chunked_body_scan` runs at ~714 MB/s. A 10 MiB body of 1-byte chunks (raw cap `header_len + MAX_BODY_SIZE`) delivered in 1 KiB pieces is ~10,240 rescans ≈ 54 GB scanned, extrapolated to **~75 s of one worker's CPU** per attacker connection.
**Fix.** Keep the scan position, decoded length and chunk state per connection (`ChunkScanState`) and resume. Also cap the number of chunks or the framing-overhead ratio.
**Probable gain.** Turns O(n²) into O(n): the ~75 s worst case becomes ~15 ms (10 MiB at ~714 MB/s, **PROJECTED**). Legitimate chunked uploads are unaffected in the common case.

### P9 · ~~HTTP pipelining is dropped~~ (FIXED 2026-09-23)
**Fixed on 2026-09-23** — see `improvements_progress.md` for the fix record. Kept below for historical record.

**Problem.** After the first response `flush_connection` sets `in_len = 0` (`connection.c:353`), discarding any bytes of a second request already received. The client sees one response for two requests.
**Fix.** Have `parse_http_request` return bytes consumed; after `flush_connection`, `memmove` the leftover to the start of `in_buf` and loop `handle_readable` until the buffer holds no complete request. Bound the loop (for example 16 requests per readiness event) for fairness.
**Probable gain.** Correctness for pipelining clients and load tools (`wrk` with a pipelining script). **ESTIMATED**: on servers that support it, pipelined GETs commonly reach 2–5× the non-pipelined request rate because syscalls per request approach 1/depth.

### P10 · ~~The accept path uses more syscalls than needed~~ (FIXED 2026-09-23)
**Fixed on 2026-09-23** — see `improvements_progress.md` for the fix record. Kept below for historical record.

**Problem.** Per accepted connection: `accept`, `fcntl(F_GETFL)`, `fcntl(F_SETFL)`, `setsockopt(TCP_NODELAY)`, then `calloc` + `malloc` (`connection.c:245-269`, `set_nonblocking` `:35`). On Linux `accept4(..., SOCK_NONBLOCK | SOCK_CLOEXEC)` does the first three in one call, and `TCP_NODELAY` set once on the listening socket is inherited by accepted sockets.
**Measured (negative result).** Removing the 64 KiB zeroing (`calloc` → `malloc`) made **no difference** to connection churn (36.4–37.8k vs 36.7–37.6k req/s, three alternating rounds), so allocation is not the bottleneck; syscalls and the kernel are. Also note macOS ephemeral-port exhaustion (TIME_WAIT) makes churn runs unreliable beyond a couple of seconds.
**Fix.** `accept4` on Linux; set `TCP_NODELAY` once on the listen socket (verify inheritance per platform); add `SO_KEEPALIVE` or an app-level keepalive for half-dead peers.
**Probable gain.** **ESTIMATED** 3 of ~8 syscalls per new connection removed on Linux, roughly 10–20% on connection-churn workloads; not measurable on macOS (no `accept4`).

---

## 5. Memory

### M1 · ~~A 64 KiB arena is allocated for every connection~~ (FIXED 2026-09-22)
**Fixed on 2026-09-22** — see `improvements_progress.md` for the fix record. Kept below for historical
record.

**Problem.** `connection_create` (`connection.c:90-106`) allocates `sizeof(Connection) + 64 KiB` for every accepted socket, even though a request runs to completion inside one event-loop turn (nothing about the arena has to outlive it except a partially-written response).
**Measured.** 5,000 idle keep-alive connections on one worker: **24,950 B per connection** with the arena; **8,415 B** with the arena buffer set to zero (everything then falls back to `malloc`), so the arena accounts for ~16.5 KB (−66%) of each connection. 121 MB → 43 MB for 5,000 connections.
**Fix.** One arena per **worker** (reset after each request), not per connection. `Connection.arena` can stay as a pointer to the shared arena so `res->conn->arena` (used in the cookbook and demo) keeps working. When a response is only partly written (`EAGAIN`), copy the unsent tail into a connection-owned buffer, and free it when drained; file-streaming chunk buffers become per-connection and lazy.
**Probable gain.** **MEASURED −16.5 KB per connection**: 10,000 idle connections ~250 MB → ~84 MB; better cache locality because requests reuse the same hot 64 KiB. Risk: any code that keeps `out_buf` across events must copy it (audit `flush_connection` and `res_send_file`).

### M2 · ~~Every idle connection owns an 8 KiB input buffer~~ (FIXED 2026-09-23)
**Fixed on 2026-09-23** — see `improvements_progress.md` for the fix record. Kept below for historical record.

**Problem.** `in_buf` is allocated at accept and lives for the connection's lifetime, growing and shrinking around it (`connection.c:95`, `:362-368`). Idle keep-alive connections, by far the common case at scale, hold it for nothing.
**Fix.** Read into a per-worker scratch buffer (16–64 KiB). If the request is complete after the read, parse it in place and never touch a per-connection buffer; only when a request is incomplete, copy the received bytes into a connection-owned buffer (allocated then, freed when the request completes).
**Probable gain.** **ESTIMATED**: together with M1 an idle connection needs only the `Connection` struct (152 B) plus its table slot, roughly **<1 KB** instead of 25 KB, so 100,000 idle connections fit in ~100 MB of application memory (kernel socket buffers are separate and not in RSS). Cost: one extra copy for requests that span several reads.

### M3 · ~~TLS connections keep large SSL buffers~~ (REMOVED 2026-09-22)
**TLS was removed from the engine entirely on 2026-09-22** — see `improvements_progress.md`. Kept for historical record.

**Problem.** `tls_init_app` sets `SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER` (`tls.c:59`) but not `SSL_MODE_RELEASE_BUFFERS`, so each `SSL` object holds its read/write buffers even while idle.
**Measured** (3,000 idle keep-alive TLS connections, each after one request): **81,230 B per connection** baseline vs **48,332 B** with `SSL_MODE_RELEASE_BUFFERS` (−33 KB, −40%). Throughput with 3,000 idle connections: 181k → 177k req/s (−3%, within run-to-run noise).
**Fix.** Add `SSL_MODE_RELEASE_BUFFERS` to `SSL_CTX_set_mode`. One token.
**Probable gain.** **MEASURED −33 KB per idle TLS connection** (~100 MB saved at 3,000 connections) for about nothing in throughput. Do it.

### M4 · ~~Request bodies are copied twice~~ (FIXED 2026-09-23)
**Fixed on 2026-09-23** — see `improvements_progress.md` for the fix record. Kept below for historical record.

**Problem.** A body lives in `in_buf` (sized to the full declared length) and is then copied into the arena by `parse_http_request` (`http_parser.c:323-326`); a 10 MiB upload peaks at ~20 MiB (and the arena copy falls back to `malloc`). `Request.body` is documented as "still a copy".
**Fix.** Point `req->body` into `in_buf`: `in_buf[in_len]` is already `'\0'` (`connection.c:436`), and since pipelining is not supported, the body ends at `in_len` for the Content-Length case. Decode chunked bodies **in place** (decoded output is never longer than the raw input).
**Probable gain.** **PROJECTED** peak memory of an upload halved (20 → 10 MiB) and one fewer 10 MiB `memcpy` (~1–2 ms). With P9 (pipelining) the body no longer necessarily ends at `in_len`; then copy only when extra bytes follow, or NUL-terminate by saving/restoring one byte.

### M5 · ~~"Streaming" responses are buffered in full~~ (FIXED 2026-09-23)
**Fixed on 2026-09-23** — see `improvements_progress.md` for the fix record. Kept below for historical
record.

**Problem.** `res_write` appends to `out_buf` in the arena (`response.c:348-375`), growing by doubling with a copy each time and keeping every old block until the request ends. Nothing is sent until the handler **returns**, so a handler cannot deliver chunks incrementally: no server-sent events, no long-poll, no large generated downloads without buffering up to 10 MiB in memory (plus the discarded doubling blocks, up to ~2×).
**Fix.** Let a streaming handler register a continuation (`res_stream(res, producer_fn, ctx)`) that the event loop calls whenever the socket is writable and the previous chunk has been sent, with a small bounded buffer. Or flush completed chunks from `res_write` when they exceed a threshold.
**Probable gain.** Memory for streamed responses bounded by the chunk size instead of the whole response (ESTIMATED), and it enables real streaming workloads. It is a feature change, not just a tuning.

### M6 · ~~Every `Route` embeds a `PATH_MAX` buffer~~ (FIXED 2026-09-23)
**Fixed on 2026-09-23** — see `improvements_progress.md` for the fix record. Kept below for historical
record.

**Problem.** `Route.static_root[PATH_MAX]` lives inside every route (`app_types.h:253`), but only `app_serve_static` routes use it. `sizeof(Route)` is 1,368 bytes on macOS (`PATH_MAX` 1024) and about 4.4 KB on Linux (`PATH_MAX` 4096).
**Fix.** Make it a `char *` (malloc'd only for static mounts).
**Probable gain.** ~4 KB per route on Linux (1,000 generated routes: ~4.4 MB → ~0.3 MB). Marginal for hand-written apps.

---

## 6. Correctness and compatibility

### C1 · ~~`Expect: 100-continue` is ignored~~ (FIXED 2026-09-23)
**Fixed on 2026-09-23** — see `improvements_progress.md` for the fix record. Kept below for historical
record.

**Problem.** The server never sends `100 Continue`. Clients such as `curl` (bodies over about 1 MB) and several HTTP libraries send `Expect: 100-continue` and wait for it before sending the body; when nothing arrives they wait a fixed timeout and then send anyway.
**Measured.** A 2 MB POST with `Expect: 100-continue`: **1.007 s**; the same request with `Expect:` suppressed: **2.4 ms** (about 420× faster).
**Fix.** When the headers are complete, the body is pending and `Expect: 100-continue` is present, validate the method/path/limits (send `413`/`417` on failure), then write `HTTP/1.1 100 Continue\r\n\r\n` once and continue reading.
**Probable gain.** Removes a one-second stall from every large upload by these clients. Cost: one small write per such request.

### C2 · ~~Path parameter names are stored per tree position~~ (FIXED 2026-09-23)
**Fixed on 2026-09-23** — see `improvements_progress.md` for the fix record. Kept below for historical
record.

**Problem.** (Finding 2 in `finds.md`.) A `:name` node keeps the name from the first route registered at that position (`router.c:459-466`); later routes with a different name at the same position capture nothing under theirs.
**Measured.** `/orders/:id/items` then `/orders/:oid/notes`: `req_get_param("oid")` returned `NULL`. `/x/*/y` followed by `/x/:id/z` captured nothing.
**Fix.** After a successful match, fill `param_names` from the matched `Route`'s own pattern (walk `route->path` for `:` segments) and take only values from the walk, or create separate param nodes per name. Add tests for both cases plus "literal beats parameter".
**Probable gain.** Correctness; a few nanoseconds per parameterized match.

### C3 · No `Date` header; a 204 carries `Content-Length`
**Problem.** Responses have `Content-Type`, `Content-Length` and `Connection` only. RFC 9110 §6.6.1 requires origin servers with a clock to send `Date`. `build_response_head` (`response.c:132-185`) also writes `Content-Length: 0` and `Content-Type: text/plain` on a `204`, which RFC 9110 §8.6 forbids (no `Content-Length` in a 204 or 1xx).
**Measured.** `GET /ping` and `DELETE` (204) responses show exactly those headers.
**Fix.** Add a cached, per-second-refreshed `Date` string (the loop already calls `time(NULL)`), and skip the framing headers for 1xx/204/304. Optionally add `X-Content-Type-Options: nosniff` for static responses.
**Probable gain.** Compliance with strict clients, caches and proxies. Cost: ~37 extra bytes per response (a `memcpy` of a cached string); no syscalls.

### C4 · ~~macOS `SO_REUSEPORT` does not balance workers~~ (FIXED 2026-09-22)
**Fixed on 2026-09-22** — see `improvements_progress.md` for the fix record. Kept below for historical
record.

**Problem.** (Finding 4 in `finds.md`.) On macOS the kernel does not spread TCP connections across the workers' listen sockets.
**Measured.** 4 workers, 5,000 connections from one client: the largest worker held 124.3 of 133.7 MB. With 1 worker `/ping` reached 221,611 req/s versus 250,055 with 4.
**Fix.** For development on macOS, either document that `WORKERS>1` does not scale, or implement a single acceptor that hands accepted fds to workers round-robin (`SCM_RIGHTS` over a socketpair), or have the master `accept` and pass the descriptor. Linux (4-tuple hashing) is expected to work as designed but was not measured.
**Probable gain.** Up to ~N× on macOS in a server-bound test (not measurable here because the client shares the machine), and balanced memory across workers.

### C5 · io_uring failure kills the server
**Problem.** `event_loop_init` failure makes `app_listen_worker` print and `exit()` (`connection.c:548-551`). Under a runtime that blocks `io_uring_setup` the process cannot start, and in cluster mode the master respawns it in a loop (S7). The epoll backend is already in the tree but only compiled with `-DCEXPRESS_USE_EPOLL`.
**Fix.** Compile both Linux backends and select at runtime: try io_uring, fall back to epoll on `ENOSYS`/`EPERM`, and log which one is active. Because `event_loop.h` is already an abstraction, this is a function-pointer table (or link-time symbol prefixing).
**Probable gain.** Works in containers and hardened kernels without special flags; no fast-path cost.

### C6 · ~~io_uring backend closes every keep-alive connection after its first request~~ (FIXED 2026-09-23)
**Fixed on 2026-09-23** — see `improvements_progress.md` for the fix record. Kept below for historical
record.

**Problem.** `event_loop_io_uring.c`'s `update_poll` (called by `event_loop_watch_write`/`unwatch_write`/`watch_read`/`unwatch_read`, all four) re-arms a connection's multishot poll by issuing `IORING_OP_POLL_REMOVE` against the existing registration, then submitting a fresh `IORING_OP_POLL_ADD` multishot in the same batch - both keyed by the same fixed `UDATA_FD(fd)` user_data. Canceling the still-active old registration generates a *third* completion for that fd, carrying `cqe->res = -ECANCELED` (-125) and no `IORING_CQE_F_MORE`, delivered with the same user_data as the live connection. `event_loop_poll`'s completion loop (`:299-333`) never inspects `cqe->flags`, and its `if (res < 0)` check (`:303`) can't tell this self-inflicted cancellation apart from a real socket error - it reports `LOOP_EVENT_ERROR`, and `connection.c`'s dispatcher closes the connection on the spot. `flush_connection` calls `event_loop_unwatch_write` after *every* keep-alive response regardless of whether write interest was ever registered (already flagged, unrelated reason, as P4), so this fires on essentially every single request on Linux.
**Measured.** Reproduced on real Linux via Docker (`docker_stress_test.sh`'s new native-arch `wrk` image, plus a raw Python socket to rule out any client quirk): a single connection sending sequential `GET /ping` requests with `Connection: keep-alive` gets a correct `200` for request 1, then `recv()` returns 0 bytes (server-initiated close) on request 2 - reproduced with `WORKERS=1` (not a multi-worker interaction) and via both container-to-container and host-mapped access (not a Docker networking artifact). Instrumenting the CQE loop directly confirms the sequence: `res=1 flags=2 more=1` (the request's real `POLLIN`) immediately followed by `res=-125 flags=0 more=0` on the same fd (the `update_poll`-triggered cancellation), then `LOOP_EVENT_ERROR` and a close. `wrk` against the same setup reports read-error counts *exceeding* the successful-request count (e.g. 207,454 errors against 205,423 requests at `-c100 -d4s`), matching every keep-alive connection dying after one request. **Not the same bug as T6's still-unexplained macOS read errors** - this reproduction is Linux/io_uring only; the kqueue backend already skips a no-op `update_poll` call via `events_watched` (`lib/CLAUDE.md`, "Event loop"), so a keep-alive response that never needed write interest never triggers kqueue's equivalent of this remove-then-add churn in the first place, and epoll's synchronous `epoll_ctl(MOD)` has no async-cancellation-completion concept to misfire this way either - this is believed to be specific to io_uring's async poll-remove/re-add pattern.
**Fix.** In `event_loop_poll`, recognize and drop a self-inflicted cancellation instead of forwarding it as `LOOP_EVENT_ERROR`: either check `cqe->flags & IORING_CQE_F_MORE` was absent *and* `res == -ECANCELED` and treat that combination as "this registration was superseded, not an error" (skip the event), or - more robust against future `update_poll` call sites - give each *generation* of a connection's poll registration its own user_data (e.g. a per-connection sequence counter folded into the encoded value) so a stale cancellation for a superseded registration is trivially distinguishable from a live one by user_data alone, not by inference over `res`/`flags`. The latter also removes the current reliance on remove-then-add ordering being race-free. Fixing P4 (skip `update_poll` entirely when the requested mask already matches `events_watched`, as the kqueue backend already does) would incidentally hide this for the common no-write-needed path, but not the underlying misclassification - a response that doesn't fully flush in one `write()` (needing real write-interest toggling) would still hit it.
**Probable gain.** Makes HTTP keep-alive actually work on Linux/io_uring, the primary target backend for real deployments of this engine. Currently every keep-alive connection behaves as if the server force-closed after one request regardless of what the client asked for, which both defeats the point of keep-alive (a fresh TCP handshake, and on Linux a fresh io_uring poll registration, per request) and would explain unexpectedly poor Linux throughput/latency numbers in any future benchmark that wasn't specifically chasing this down.

---

## 7. Testing and tooling gaps behind the bugs above

| ID | Gap | Fix |
|---|---|---|
| T1 | ~~No test sends a malformed **request line**~~ **PARTIALLY FIXED 2026-09-23**: `test_http_hardening.c` and `test_connection.c` now cover it (S8's fix), see `improvements_progress.md`; the fuzzer still checks memory safety only, not "every input is answered or closed" | A fuzzer oracle that asserts `request_is_complete` eventually returns 1 for terminated garbage remains open |
| T2 | No test for "literal beats `:param`", different parameter names at one position, `%00`, over-long headers, 33 headers | Add to `test_router.c` / `test_http_hardening.c` (C2, S5, S6) |
| T3 | `tests/test_router.c` frees only `app->connections`, so route trees leak; a Linux LeakSanitizer run would report them | Call `app_free_routes` in `cleanup_app` |
| T4 | `scripts/stress_test.sh` starts the demo from the repo root (its `GET /` measures a 404) and its memory sampler disagrees with direct measurement | `cd examples/todo_sqlite` before launching; verify the sampler against `ps` |
| T5 | No Linux run of any test, benchmark or io_uring code path in this environment | Add a CI job on Linux (build + `make test` + sanitizers + a `wrk` smoke run); every ESTIMATED tag above needs it. Partially closed 2026-09-22: a manual Docker run (`docker_stress_test.sh`) found C6 (io_uring closes every keep-alive connection after one request) on the first real Linux exercise of this codebase - exactly the kind of bug this gap predicted. A real CI job, not an ad-hoc manual run, is still needed |
| T6 | Read errors at 5,000 connections (hundreds to a couple thousand per run) are unexplained | Run the same `wrk -c5000` against a trivial known-good server (a 30-line kqueue echo, or nginx) on the same machine: if it shows the same errors, it is the OS or `wrk`, not CExpress; otherwise log `errno` at every `connection_close` caused by `LOOP_EVENT_ERROR` to see which condition drops connections. Also raise `BACKLOG` and compare |
| T7 | `make bench` has no static-file, JSON-list-through-response, or many-routes case | Add the P1/P7 experiments as benchmark cases so future changes are measured |

---

## 8. Things I measured that are NOT worth doing

Listed so you do not spend time on them.

- **Writing JSON into the arena instead of libc** (`yyjson_mut_write_opts` with the arena allocator): 1,047–1,063 ns vs 1,024–1,039 ns per 20-row response (2–4%, within noise). The libc `malloc`/`free` of the serialized string is not a bottleneck. The only reason to do it is safety: it removes the "forgot to `free`" leak trap.
- **Avoiding the `calloc` zeroing of the arena**: no change in connection churn (P10).
- **`SSL_MODE_RELEASE_BUFFERS` as a speed change**: it is neutral for throughput (−3%, noise); do it for memory (M3) only.
- **Raising `MAX_EVENTS` (64)**: at the measured rates each `kevent`/`epoll_wait` returns a handful of events; expected effect under 2% (ESTIMATED, not tested).

---

## 9. How the numbers were produced

All on an Apple M3 Pro laptop, macOS, gcc-16 -O2, 21 Sep 2026. Servers were the current `examples/todo_sqlite` demo (or scratch variants of it) started from `examples/todo_sqlite/` with `QUIET=1`.

| Experiment | Method |
|---|---|
| P1 | Scratch server with `/mem` (`res_send_bytes` of the 6,481-byte page) and `/file` (`res_send_file`); `wrk -t8 -c100 -d10s`, two alternating rounds; `/static/*` against the demo with `-d8s` |
| P2, P3 | Micro-benchmark over `request_is_complete`, `parse_http_request` and bare `phr_parse_request` (1,000,000 iterations each), using the request strings from `tests/bench_hotpath.c` |
| P5 | TLS demo (`tests/certs`), 1 worker; a script held 3,000 handshaken keep-alive connections (one request each) while `wrk -t2 -c50 -d8s https://…/ping` ran; variants rebuilt from copies of `connection.c` (scan restricted to the batch's events) and `tls.c` (`SSL_MODE_RELEASE_BUFFERS`) |
| P7, P8 | C program calling `match_route` (1,000,000 lookups) and `chunked_body_scan` over 200,000 one-byte chunks; the attacker figure is arithmetic on the measured scan rate |
| P10 | Demo variant with `malloc`+partial `memset` in `connection_create`; `wrk -t4 -c50 -d2s -H "Connection: close"`, three alternating rounds, waiting for TIME_WAIT to drain between runs |
| M1 | 5,000 idle keep-alive connections held by a script; `ps` RSS delta per connection; baseline vs a build with `ARENA_SIZE 0` |
| M3 | As P5, RSS delta per connection after 3,000 TLS connections |
| S1 | A script sent one byte every 20 s to a 1-worker server for 168 s |
| S3 | Server started under `ulimit -n 40`, 100 clients connected |
| S4 | 300 connections, each sending a 10 MiB `Content-Length` and 9 KB of body; `ps` RSS and VSZ |
| S6, C1, C3 | `curl` against the running demo (`%00` path; a 2 MB POST with and without `Expect: 100-continue`; response headers) |
| S7 | Port held by another listener, `WORKERS=2`, counted `respawning` lines in the server log after 4 s |
| S8 | Raw-socket script against the running demo (also in `finds.md`) |
| C2 | Scratch program registering conflicting patterns and calling `match_route` + `req_get_param` |
| C4 | 4-worker cluster, script opening 100 / 1,000 / 5,000 connections, `ps` RSS per process |
| C6 | **The one Linux measurement in this document** (2026-09-22, via Docker on the same M3 Pro, `docker_stress_test.sh`'s native-arch `wrk` image against the real io_uring backend): `wrk` and a raw Python socket script against a single `GET /ping` connection with `Connection: keep-alive`; temporary `fprintf` instrumentation in `event_loop_io_uring.c`'s CQE loop and `connection.c`'s `LOOP_EVENT_ERROR` branch (added, used, then reverted - no trace left in the diff) to capture the exact `res`/`flags` sequence |

Limits of this evidence: single runs; client and server share cores; macOS only except C6 (Linux, via Docker); network effects (real latency, real packet loss) absent. Re-run anything you plan to rely on for a decision.