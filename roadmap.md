# Roadmap: lib/ review follow-ups

Open work from the file-by-file review of `lib/` on 2026-09-29: security, performance, and how the files talk to
each other. Items are in the suggested order: security bugs with small fixes first, then process and listener
lifecycle, then performance, then hardening. Pick the first unchecked item.

**Confirmed** means a scratch program reproduced it on macOS (gcc-16, kqueue) on 2026-09-29. **From code** means it
was found by reading and not run. Nothing here was run on Linux. Line links are as of that date and will drift; the
function names are the stable reference.

## How to work an item

1. **Test first.** Write the failing test in the suite the item names, then fix. Test files stay under 1,000 lines
   (`tests/CLAUDE.md`): `test_response.c` (961), `test_http_parser.c` (1,020), `test_router.c` (1,041) and
   `test_connection.c` (2,227) have no room, so tests for those areas go in another suite or a new one. A new suite
   needs a `*_TEST_BIN` variable, a link rule listing the objects it needs and an entry in `TEST_BINS` in the
   `Makefile`; copy how `test_stream` is wired.
2. **Fix** within the hot-path rules in [lib/CLAUDE.md](lib/CLAUDE.md) ("Hot-path rules").
3. **Verify:** `make test`, `make SANITIZE=1 BUILD_DIR=build-asan test` and `make check-docs` every time; `make fuzz`
   after touching `http_parser.c`, `router.c`, `response.c` or `arena.c`; `make bench` after a hot-path change;
   `make test_epoll` after a `connection.c` or event-loop change; Docker for Linux-only behavior (`tests/CLAUDE.md`,
   "Running").
4. **Record:** a `CHANGELOG.md` "Unreleased" entry for any behavior change; in `lib/CLAUDE.md`, delete or trim the
   item's "Known gaps" entry and describe the new rule where that behavior is documented; describe the new test in
   `tests/CLAUDE.md`; tick the box below.
5. **Name behavior, not items.** Code, comments, test names and commit messages say what changed ("an empty chunk no
   longer ends the body"), never "roadmap item N".

## Checklist

Security, all confirmed:
- [x] [An empty res_write ends a chunked body early](#an-empty-res_write-ends-a-chunked-body-early)
- [ ] [Mixing whole-body and chunked sends corrupts framing](#mixing-whole-body-and-chunked-sends-corrupts-framing)
- [ ] [res_set_header accepts Transfer-Encoding](#res_set_header-accepts-transfer-encoding)
- [ ] [Opening a file to stream can block the worker](#opening-a-file-to-stream-can-block-the-worker)
- [ ] [Case variants bypass prefix middleware on static mounts](#case-variants-bypass-prefix-middleware-on-static-mounts)

Processes and listeners:
- [ ] [macOS workers spin when the master dies](#macos-workers-spin-when-the-master-dies)
- [ ] [SIGINT to the master alone skips the drain](#sigint-to-the-master-alone-skips-the-drain)
- [ ] [The master's accept loop starves supervision](#the-masters-accept-loop-starves-supervision)
- [ ] [Standalone servers silently share a port](#standalone-servers-silently-share-a-port)

Performance:
- [ ] [The head is re-parsed on every recv while a body arrives](#the-head-is-re-parsed-on-every-recv-while-a-body-arrives)
- [ ] [Chunked bodies are scanned twice](#chunked-bodies-are-scanned-twice)
- [ ] [Small per-request costs on the hot path](#small-per-request-costs-on-the-hot-path)
- [ ] [Arena overflow mallocs every allocation](#arena-overflow-mallocs-every-allocation)

Hardening:
- [ ] [Path parameters over 63 bytes are truncated](#path-parameters-over-63-bytes-are-truncated)
- [ ] [Registration silently truncates long mount paths and prefixes](#registration-silently-truncates-long-mount-paths-and-prefixes)

Lower priority: [Minor backlog](#minor-backlog). Reference: [No action planned](#no-action-planned),
[Verified sound](#verified-sound).

---

## Security

### An empty res_write ends a chunked body early

**Status:** fixed 2026-09-29; test in `tests/test_response_framing.c`. **Where:** `res_write`,
[lib/response.c:587](lib/response.c#L587).

`res_write(res, "", 0)` frames a zero-length chunk, and `0\r\n\r\n` is the last-chunk marker, so the client sees the
body end there. Everything written afterwards, `res_end`'s real terminator included, reaches a keep-alive client as
the start of a bogus next response. Only `len == 0` with `data == NULL` is a no-op today, and an empty string is easy
to hit when writing rows or buffers in a loop.

```c
res_write(res, "ab", 2); res_write(res, "", 0); res_write(res, "cd", 2); res_end(res);
/* body on the wire: 2\r\nab\r\n 0\r\n\r\n 2\r\ncd\r\n 0\r\n\r\n */
```

**Fix:** once the head is committed, return 0 for any `len == 0`, whatever `data` is. `stream_write` already does this.

**Test:** in a new response suite: the calls above decode to `abcd` with exactly one `0\r\n\r\n`, at the end.

### Mixing whole-body and chunked sends corrupts framing

**Status:** confirmed, four variants. **Where:** `send_with_content_type`, [lib/response.c:260](lib/response.c#L260);
`commit_chunked_headers`, [lib/response.c:516](lib/response.c#L516); `res_stream`,
[lib/response.c:710](lib/response.c#L710).

Whole-body sends (`res_send`, `res_json`, `res_send_bytes`, `res_redirect`, `res_send_shared`, `res_send_file`) build
a new `out_buf` but leave the chunked state (`headers_sent`, `stream_ended`) as it was, and `commit_chunked_headers`
appends to whatever `out_buf` holds:

| One handler calls | What goes on the wire |
|---|---|
| `res_send`, then `res_write` + `res_end` | two complete responses for one request |
| `res_send`, then `res_stream` | the first response, then a second chunked head followed by the producer's body |
| `res_write`, then `res_send` of a 500, then `res_end` | the 500, then a stray `0\r\n\r\n` after its `Content-Length` body |
| `res_send_file`, then `res_write` | chunk bytes between the head and the file body, counted against its `Content-Length` |

The third row is a natural error-path shape. Each one desyncs a keep-alive client or a proxy in front.

**Fix (recommended rule):** whole-body sends keep replacing each other (last wins, as documented today), and once one
has run the chunked calls are closed: `res_write` returns -1, `res_end` does nothing, and `res_stream` returns -1 so
the caller keeps its ctx. `fail_chunked_response` already leaves a Response in that state. Concretely,
`send_with_content_type` sets `stream_ended = 1` and `is_chunked = 0` once the head is built, and `res_stream` also
refuses when `stream_ended` is set. `commit_chunked_headers` is then only reached with no response built; have it
reset `out_buf` rather than append, as a guard.

**Decision to confirm:** the alternative, last wins for everything, would let a stray `res_end` after an error
`res_send` replace the error with an empty chunked 200.

**Test:** in `tests/test_response_framing.c`: every row above yields exactly one response; `res_stream` after `res_send`
returns -1 and never calls `ctx_free`; a second `res_send` still replaces the first; `test_stream.c`'s "a later
`res_send` replaces a stream" case still passes. Update the contract comment at the top of `lib/response.h`, and
`lib/API.md` if a one-liner states it.

### res_set_header accepts Transfer-Encoding

**Status:** confirmed. **Where:** `res_set_header`, [lib/response.c:118](lib/response.c#L118); the header loop in
`build_response_head`, [lib/response.c:220](lib/response.c#L220).

`Content-Length`, `Connection` and `Date` are refused as engine-owned, but `Transfer-Encoding` is not, and
`build_response_head` filters it only on chunked and bodiless heads. A fixed-length response then carries both
`Content-Length: 5` and `Transfer-Encoding: chunked`, the request-smuggling shape: a proxy that trusts
`Transfer-Encoding` reads the body as chunks.

**Fix:** refuse `Transfer-Encoding` in `res_set_header` like the other engine-owned names, logged. The
`Content-Length` and `Connection` checks in `build_response_head`'s loop are already unreachable, since
`res_set_header` refuses both.

**Test:** in `tests/test_response_framing.c`: `res_set_header(res, "transfer-encoding", "chunked")`, in any letter case, then
`res_send`: no `Transfer-Encoding` line, `Content-Length` present.

### Opening a file to stream can block the worker

**Status:** confirmed: `res_send_file` on a FIFO blocked until a 2-second alarm killed the probe. **Where:**
`res_send_file`, [lib/response.c:660](lib/response.c#L660); the small-file read in `static_serve_file`,
[lib/static.c:419](lib/static.c#L419).

`open(filepath, O_RDONLY)` blocks on a FIFO with no writer, and a blocked `open` freezes the whole single-threaded
worker; the regular-file check (`fstat`) only runs after `open` returns. `static_serve_file` stats the file first but
opens it again by path afterwards (`res_send_file`, or `fopen` for cacheable sizes), so a file replaced by a FIFO in
between hangs the worker too. That race needs write access inside the static root. The fd `res_send_file` keeps
across event-loop turns also lacks `O_CLOEXEC`, so a handler that forks and execs meanwhile leaks it into the child.

**Fix:** `open(filepath, O_RDONLY | O_NONBLOCK | O_CLOEXEC)`. `O_NONBLOCK` changes nothing for regular-file reads,
`sendfile` or `pread`, and the existing `fstat` check then refuses the FIFO. In `static.c`, replace
`fopen`/`fread` with the same `open`, an `fstat` that checks for a regular file with the same `st_dev`/`st_ino` as
the earlier `stat`, and a `read` loop. Optional: `O_NOFOLLOW` on the static path only, since the realpath-resolved
name never ends in a symlink. `res_send_file` itself is public and may be handed symlinks on purpose.

**Test:** `test_static.c`: `mkfifo` in the sandbox; `res_send_file` on it returns -1 promptly. Guard the call with
`alarm` so a regression fails instead of hanging `make test`.

### Case variants bypass prefix middleware on static mounts

**Status:** confirmed on macOS APFS. **Where:** `static_serve_file`, [lib/static.c:299](lib/static.c#L299), against
the byte-exact prefix match in `chain_next`, [lib/middleware.c:49](lib/middleware.c#L49).

On a case-insensitive filesystem, `GET /static/PRIVATE/secret.txt` does not match
`app_use_prefix(app, "/static/private", auth)`, so `auth` never runs, yet the static mount resolves it to
`private/secret.txt` and answers 200 with the file. macOS `realpath` returns the on-disk spelling
(`.../private/secret.txt`), which is what makes a fix possible. APFS is also normalization-insensitive, so the
composed and decomposed spellings of a non-ASCII name are the same file; same shape, not probed. Linux ext4 and xfs
are case-sensitive and unaffected.

**Fix, decide first:**
- **Strict**, fits deny by default: answer 404 unless the resolved path equals the candidate byte for byte. Closes
  case and normalization variants and any symlink combination, but symlinks inside the root stop working (hard links
  still do), and the symlink case in `test_cache_aliases_share_one_body` must change.
- **Narrow:** answer 404 only when resolved and candidate differ by ASCII letter case alone. Keeps symlinks; leaves a
  symlink combined with a case variant, and normalization variants, open.

Either check goes in the miss path after `resolve_and_stat`, and after the `index.html` retry, before `cache_insert`,
so a refused variant is never cached. The cache's no-syscall fast path only finds candidates inserted earlier.

**Test:** `test_static.c`, skipped with a printed note when the sandbox filesystem is case-sensitive (create `a`,
stat `A`): the case variant answers 404 and adds no cache entry.

## Processes and listeners

### macOS workers spin when the master dies

**Status:** confirmed: after the master's end of the control socket closed, 100,000 loop turns ran in 0.058 s; with
it open, 3 turns took 2 s. **Where:** `accept_passed_connections`,
[lib/connection.c:675](lib/connection.c#L675); kqueue's classification of the listen fd,
[lib/event_loop_kqueue.c:214](lib/event_loop_kqueue.c#L214).

Under the single acceptor, a worker's `server_fd` is its control socket to the master. If the master dies without
draining (SIGKILL, crash, OOM kill), the socket reports EOF, kqueue keeps it readable, every turn becomes
`LOOP_EVENT_ACCEPT`, and `recvmsg` returns 0 so the function just returns. The worker burns a core forever and gets
no more connections.

**Fix:** treat `recvmsg` returning 0, or failing with anything but `EAGAIN`, `EWOULDBLOCK` or `EINTR`, as "master
gone": log it and call `app_stop(app)`, which already unwatches and closes `server_fd`, drains, and ends the loop.

**Test:** `test_cluster.c`, inside `#ifdef CEXPRESS_SINGLE_ACCEPTOR`: an `App` with `accept_via_fd_passing` on one
end of a socketpair; close the other end; `app_run_once` must reach `LOOP_TURN_EXIT` within a few turns.

### SIGINT to the master alone skips the drain

**Status:** from code. **Where:** the shutdown sequence in `cluster_listen`, [lib/cluster.c:520](lib/cluster.c#L520).

The master forwards only SIGTERM, assuming a SIGINT came from the terminal and reached the whole process group.
`kill -INT <master>`, or a supervisor whose stop signal is SIGINT, reaches the master alone: workers keep serving,
and the master waits out its 6-second deadline and SIGKILLs them mid-request.

**Trap:** don't just forward SIGINT too. A worker that already got the terminal's SIGINT treats the forwarded signal
as its second one and exits at once, skipping its own drain.

**Fix:** put each worker in its own process group, with `setpgid(0, 0)` in the child and `setpgid(pid, pid)` in the
parent to close the race, so terminal signals reach only the master. The master then sends every worker exactly one
SIGTERM, whatever signal it got. Consequences to handle: workers no longer receive terminal SIGHUP or SIGQUIT, so the
master must turn SIGHUP into the same drain, and `kill -- -<pgid>` reaches only the master, which relays it.

**Test:** `test_cluster.c`: fork a real master, as the existing lifecycle tests do; `kill(master, SIGINT)`; assert it
exits 0 in well under the 6-second kill deadline. Today it takes at least 6 seconds.

### The master's accept loop starves supervision

**Status:** from code. **Where:** the single-acceptor drain in `cluster_listen`, [lib/cluster.c:479](lib/cluster.c#L479)
(macOS and BSD).

After `poll` reports the listen socket readable, the master accepts until `EAGAIN`. Under a sustained connection
flood the backlog never empties, so the master never gets back to `waitpid`, respawns or `g_shutdown_requested`:
crashed workers stay down, and SIGTERM does not start a drain until the flood stops.

**Fix:** cap accepts per wake, for example at `MAX_EVENTS`, then fall through to the supervision checks. Also stop
draining once `g_shutdown_requested` is set.

**Test:** there is no natural unit test while the loop is inline. Either move the drain into a helper that takes a
cap and test that it returns after the cap with more connections pending, or check with a scratch flood
(`wrk -c5000` against a two-worker cluster) that SIGTERM still drains promptly.

### Standalone servers silently share a port

**Status:** confirmed: a second `create_server_socket` on the same address and port succeeded in the same process.
**Where:** `create_server_socket`, [lib/connection.c:185](lib/connection.c#L185).

`SO_REUSEPORT` is set on every listener, though only Linux cluster workers need it. A second copy of the server
started by mistake splits traffic with the first instead of failing with `EADDRINUSE`, and on Linux any other process
running as the same user can bind the port and take a share of the connections.

**Fix:** make port sharing explicit. Listeners are exclusive by default, and only Linux cluster workers ask for
`SO_REUSEPORT`, through an extra parameter or a separate function for that one caller; a new public function goes in
`lib/API.md` (`make check-docs` enforces it). The Linux preflight bind then also catches a port held by another
`SO_REUSEPORT` user. The macOS single acceptor needs no sharing at all.

**Test:** `test_listen.c`: two `create_server_socket` calls on the same address and port; the second returns -1.
`test_cluster.c`'s Linux multi-worker tests must still pass, in Docker.

## Performance

### The head is re-parsed on every recv while a body arrives

**Status:** confirmed and measured: one parse of a 7.3 KB head with 99 headers costs 5.3 µs, against 26 ns for a
minimal head. **Where:** `parse_request_head_resume`, [lib/http_parser.c:388](lib/http_parser.c#L388), called on
every pass of `serve_buffered_requests`, [lib/connection.c:1565](lib/connection.c#L1565).

`head_scan` only advances while no blank line has been found; after a successful parse it stays 0. Every later
`recv` of the same request therefore runs `phr_parse_request`, the bare-LF scan and the framing pass over the whole
head again. A client that sends a large head and then drips its body in small segments makes the worker pay about
5 µs of head parsing per segment. With the framing verdict kept, that drops to a length comparison.

**Fix:** once `header_len > 0` and the body is still short, keep the framing verdict on the `Connection` (header
length, `Content-Length`, chunked flag, `Expect` bits, minor version) and skip the head parse until the body is
complete. Parse the head once more at dispatch, where `parse_http_request_in_place` needs the method, path and header
views. Reset the kept verdict with `head_scan` and `chunk_scan` in `finish_request`.

**Test:** behavior must not change, so `test_answered.c` (every split point) and `make fuzz` (the incremental path
decides "complete" at the same prefix as a from-scratch scan) must pass untouched. Add a `bench_hotpath.c` case
with a large head and a body in many small reads to show the per-read cost dropping.

### Chunked bodies are scanned twice

**Status:** from code. **Where:** `parse_http_request_in_place`, [lib/http_parser.c:720](lib/http_parser.c#L720).

`request_head_is_complete` validates a chunked body incrementally (`Connection.chunk_scan`), then the in-place parse
runs `chunked_body_scan` from the first byte again before `chunked_body_decode` walks the body a third time. This is
negligible for small bodies. For the documented worst case, 10 MiB of 1-byte chunks at about 14 ms per scan, it is a
whole extra scan.

**Fix:** pass the completed `ChunkScanState` (its `decoded_len` and `body_end`) into the engine's in-place parse and
skip the rescan. `parse_http_request` and `parse_http_request_from_head`, used by tests and tools, keep scanning from
scratch. This is safe because a resumed scan's verdict equals a from-scratch one, which `fuzz_parser.c` already
asserts.

**Test:** the existing chunked tests and `make fuzz`; add a `bench_hotpath.c` case for a large chunked body.

### Small per-request costs on the hot path

**Status:** from code; measure with `make bench` before and after.

- `request_wants_close`, [lib/http_parser.c:512](lib/http_parser.c#L512), runs on every request and copies the
  `Connection` header into the arena through `req_get_header`. Match the token on the header view instead.
- `parse_request_fields` builds `req->version` with `snprintf`, [lib/http_parser.c:626](lib/http_parser.c#L626).
  picohttpparser's minor version is one digit, so copy `HTTP/1.` and append it.
- `res_write` and `stream_write` format the chunk-size line with `snprintf`,
  [lib/response.c:592](lib/response.c#L592) and [lib/response.c:744](lib/response.c#L744). A small hex formatter
  like `put_uint` does it without the format parser.

### Arena overflow mallocs every allocation

**Status:** from code; measure before changing anything. **Where:** `arena_alloc`,
[lib/arena.c:34](lib/arena.c#L34).

Once the 64 KiB block is full, every further allocation, however small, is its own `malloc` chained on `large` and
freed at reset. Handlers that make many small allocations past the first 64 KiB, such as header copies or their own
`arena_alloc` calls, pay one `malloc`/`free` pair each. yyjson allocates in growing chunks, so it is less affected.

**Fix, only if a benchmark shows it matters:** when the block is full, take another `ARENA_SIZE` block from
`App.arena_pool` and bump-allocate in it, keeping a dedicated `malloc` only for single allocations larger than a
block. This touches `arena_grow`'s in-place rules, `large_bytes` (charged to the memory budget for deferred
requests), `arena_hand_over` and `test_arena.c`.

## Hardening

### Path parameters over 63 bytes are truncated

**Status:** from code; already in `lib/CLAUDE.md` "Known gaps". **Where:** `match_path`,
[lib/router.c:496](lib/router.c#L496).

A captured `:param` longer than 63 bytes is cut to 63 without any error, so the handler looks up a different
identifier than the client sent, and two long ids that share their first 63 bytes become the same id. Query names and
values (63 bytes) and the raw query (255 bytes) have the same shape.

**Fix:** copy captured values into the request arena at full length and keep `req_get_param`'s signature; the path
is capped at 255 bytes, so the size stays bounded. Alternatively, refuse the request instead of truncating, the way
`parse_urlencoded_body` returns -2. Arena copies must move with a deferred request: `request_clone_used`,
[lib/http_parser.c:834](lib/http_parser.c#L834), copies the fixed parameter slots today.

**Test:** the router suite has no room, so a new or smaller suite: a 100-byte segment round-trips exactly through
`req_get_param`, including after `res_defer`, through `req_deferred`.

### Registration silently truncates long mount paths and prefixes

**Status:** from code. **Where:** `build_mounted_path` and `app_mount`, [lib/router.c:346](lib/router.c#L346);
`path_normalize_prefix`, [lib/http_parser.c:126](lib/http_parser.c#L126).

`fill_route` refuses a pattern that doesn't fit instead of registering a shorter one, but `app_mount` builds prefix
plus path with `snprintf` into 256 bytes and passes the cut result on, which registers a different route.
`path_normalize_prefix` also cuts prefixes longer than 127 bytes at a segment boundary for `app_use_prefix`,
`app_use_body_limit`, `app_mount` and `app_serve_static`, silently widening what they cover.

**Fix:** check `snprintf`'s result in `build_mounted_path`, and have `path_normalize_prefix` report truncation so each
caller refuses the registration with a `stderr` line, like `fill_route`.

**Test:** `test_middleware.c` or a new suite: an over-long mount registers nothing, and neither does an over-long
prefix.

## Minor backlog

Found in the same review and left out of its summary as low impact. None of these were probed.

- **The Allow header is cut mid-name.** `dispatch` collects allowed methods into 64 bytes,
  [lib/middleware.c:116](lib/middleware.c#L116). The seven standard methods fit; many custom methods get cut in the
  middle of a name.
- **Header names aren't checked as tokens.** `res_set_header` refuses control characters and `:` but accepts spaces
  and other non-token characters in a name, [lib/response.c:124](lib/response.c#L124), which yields a malformed head.
- **Descriptors without close-on-exec.** The listen socket, [lib/connection.c:158](lib/connection.c#L158), the spare
  `/dev/null` descriptor, [lib/router.c:19](lib/router.c#L19) and [lib/connection.c:378](lib/connection.c#L378),
  accepted sockets on macOS, [lib/connection.c:237](lib/connection.c#L237), and fds passed to workers all survive
  `exec` in a child that a handler starts.
- **Decoded CR and LF reach the path.** `request_target_path` refuses a decoded NUL but no other control byte, so
  `%0d%0a` lands in `req->path`. Response headers already drop control characters; what is left is log injection in
  apps that log paths.
- **The macOS master keeps its listen socket open while draining.** For up to 6 seconds the kernel keeps completing
  handshakes that nobody will serve, then resets them, [lib/cluster.c:569](lib/cluster.c#L569). Closing it when the
  drain starts refuses them at once.
- **io_uring never retries a failed poll arm.** `update_poll` records the new mask before `arm_poll`, so if no
  submission entry is available the fd stays unarmed and later watches with the same mask are no-ops,
  [lib/event_loop_io_uring.c:90](lib/event_loop_io_uring.c#L90).
- **Linux workers outlive a dead master.** Nothing sets a parent-death signal, so after a master crash the workers
  keep serving with nobody to restart or stop them. Decide whether that is wanted; systemd's control-group kill and
  container runtimes clean them up anyway.

## No action planned

- **A stale event can reach a new connection.** `handle_event` drops an event whose `Connection` pointer no longer
  matches `App.connections[fd]`. If a connection closes and a new one reuses both its fd number and its freed address
  within one event batch, a leftover event reaches the new connection. Handlers already tolerate spurious readiness,
  so the cost is one wasted wakeup. A per-connection generation in `LoopEvent` would remove it if it ever matters.

## Verified sound

Checked during the review; no need to re-audit unless the code changes.

- Arena ownership across deferred responses, coalesced batches and `EAGAIN` tail copies.
- Restoring the byte that the body's NUL terminator overwrites on pipelined requests.
- One canonical path shared by the parser, middleware, body limits and static mounts, apart from the
  case-insensitive filesystem gap above.
- Event-loop interest tracking on kqueue, epoll and io_uring.
- Buffered-memory budget accounting.
- Parser strictness on `Content-Length`, `Transfer-Encoding`, `Host`, bare LF and chunk-size lines; multipart
  parameter parsing; urlencoded decoded-length limits; arena size-overflow guards.
