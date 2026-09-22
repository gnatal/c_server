# Improvements progress

Tracks work done against [`improvements.md`](improvements.md), one entry per item, in the order tackled.

---

## S1 · Slow-drip clients hold connections forever

**Date completed.** 2026-09-22.

**How it was completed.**

Added a second, activity-independent deadline clock alongside the existing `last_activity`-based
60 s idle timeout, which a slow-drip client (one byte every N < 60 s) never trips because every byte
refreshes `last_activity`.

- `lib/app_types.h`: added `Connection.request_started` (a `time_t`, zero-initialized by the
  existing `calloc` in `connection_create`) and two new limits, `REQUEST_HEADER_TIMEOUT_SECONDS`
  (10) and `REQUEST_BODY_TIMEOUT_SECONDS` (30).
- `lib/connection.c`:
  - `handle_readable` arms `request_started = time(NULL)` the first time it runs after the
    connection went idle (`request_started == 0`), i.e. on the first byte of a fresh request. It is
    **not** refreshed on subsequent bytes, unlike `last_activity` — that's what makes it immune to
    the drip.
  - `flush_connection`'s keep-alive branch clears `request_started` back to `0` once a response has
    been fully queued, so a connection idling *between* requests is still governed only by the
    original `last_activity` / `IDLE_TIMEOUT_SECONDS` check (no regression for ordinary keep-alive
    clients that pause between requests).
  - `close_idle_connections` gained a new branch, checked before the existing silence-based one: if
    `request_started != 0`, it determines whether headers are complete yet with a cheap
    `request_framing` call (bounded by `in_len`, ≤ 8 KiB unless a body is already being buffered, in
    which case headers are trivially known to be complete already) and applies
    `REQUEST_HEADER_TIMEOUT_SECONDS` or the more generous `REQUEST_BODY_TIMEOUT_SECONDS`
    accordingly. Expiry sends 408 if any bytes were received (`in_len > 0`), otherwise just closes
    (nothing to answer with, e.g. a stalled TLS handshake with zero application bytes). This call is
    on the once-a-second sweep over all connections, not the per-byte hot path, so the extra parse is
    negligible (same cost class the sweep already pays per connection).
- `lib/tls.c`: `tls_connection_init` arms `request_started` when a handshake begins (covers a client
  that opens the TCP connection and never sends a ClientHello, or stalls partway through), and
  `tls_connection_handshake` re-arms it to `time(NULL)` on handshake success, so time spent in the
  handshake doesn't eat into the header deadline for the request that follows.

Deliberately *not* done, to keep this a same-day (S) fix: request_started is **not** set at
`connection_create`/accept time for plaintext connections. A connection that is accepted and then
sends nothing at all is unaffected by this change and still relies on the pre-existing 60 s
`IDLE_TIMEOUT_SECONDS` sweep — S1 is specifically about a request that *is* progressing, too slowly,
not one that never starts. `improvements.md`'s suggested body-deadline shape ("a total limit, or a
minimum-rate rule such as 1 KB/s after a grace period") was simplified to a flat 30 s cap from
`request_started` rather than a rate rule, since the existing state (`request_started`,
`request_framing`) was enough to implement without adding a bytes-at-last-check tracking field; a
rate rule remains a possible follow-up if a legitimate large upload needs more sustained but very
slow throughput than 30 s allows for 10 MiB.

**Tests and results.**

- Unit tests added to `tests/test_connection.c` (registered in `main`):
  - `test_close_idle_connections_header_deadline_closes_slow_drip` — reproduces the exact slow-drip
    shape (a byte "just" arrived, refreshing `last_activity`, but the request has actually been
    dribbling in longer than the header deadline) and asserts an explicit 408 plus connection
    reclaim.
  - `test_close_idle_connections_header_deadline_leaves_fresh_partial_request_alone` — a partial
    request still within the header deadline must not be touched.
  - `test_close_idle_connections_body_deadline_allows_slow_body_within_window` — past the header
    deadline but within the body deadline (headers already complete, body pending) must survive.
  - `test_close_idle_connections_body_deadline_closes_stalled_body` — past the body deadline gets
    408'd.
  - Updated the pre-existing `test_close_idle_connections_408s_stalled_partial_request` to also
    backdate `request_started` (it previously only backdated `last_activity`, which the new
    `request_started` check now intercepts first).
- Unit test added to `tests/test_tls.c`:
  - `test_tls_handshake_deadline_closes_stalled_handshake` — starts a TLS handshake
    (`tls_connection_init`) with no ClientHello ever sent, backdates `request_started`, and asserts
    the connection is reclaimed by `close_idle_connections`.
- `make test`: all 14 suites pass, including the 5 new/updated cases above.
- `make SANITIZE=1 BUILD_DIR=build-asan test`: all 14 suites pass clean under ASan + UBSan (no new
  findings from the changed code).
- Compiled `lib/connection.c` directly with the project's `-Wall -Wextra -std=c11 -O2` flags: no new
  warnings.
- Live end-to-end reproduction against `examples/todo_sqlite`'s demo server (`make demo`, run from
  `examples/todo_sqlite/`): a raw socket sent `GET /ping HTTP/1.1\r\n` one byte every 3 s (never
  completing the header block). Before this fix (per `improvements.md`, S1) the same shape of
  client — one byte every 20 s — was still connected after 168 s. After the fix:

  ```
  [ 0.0s] sent byte 1/20
  [ 4.0s] sent byte 2/20
  [ 8.0s] sent byte 3/20
  [12.0s] sent byte 4/20
  [12.0s] received: HTTP/1.1 408 Request Timeout ... Connection: close
  RESULT: connection closed after 12.0s
  ```

  (10 s `REQUEST_HEADER_TIMEOUT_SECONDS` deadline plus up to ~1 s sweep granularity plus the 1 s
  byte-send tick before the sweep next runs — consistent with the configured deadline.) Verified
  normal traffic (`GET /ping`, `GET /api/todos`) still returns 200 immediately afterward on the same
  server, confirming no regression to legitimate requests or to the normal keep-alive idle path.

**Status:** Fixed. `REQUEST_HEADER_TIMEOUT_SECONDS` / `REQUEST_BODY_TIMEOUT_SECONDS` are compile-time
constants in `lib/app_types.h`, consistent with this repo's existing "limits are constants, not
runtime knobs" convention — not exposed as `ServerConfig` fields.

---

## S2 · A client that stops reading holds its connection forever

**Date completed.** 2026-09-22.

**How it was completed.**

`close_idle_connections` used to skip any connection with a response pending (`out_buf != NULL` or
`file_fd >= 0`) unconditionally — a client that requested a large response and then simply stopped
reading kept its fd, arena and full output buffer forever, with no mechanism to reclaim it. Added a
third clock, alongside S1's `request_started`, that specifically measures write *progress* rather than
write *activity*.

- `lib/app_types.h`: added `Connection.last_write_progress` (a `time_t`, zero-initialized by the
  existing `calloc`) and `WRITE_TIMEOUT_SECONDS` (30).
- `lib/connection.c`:
  - `flush_connection` arms `last_write_progress = time(NULL)` the first time it runs for a given
    response (even before any byte is actually written — a response that gets `EAGAIN` on every
    attempt is exactly the "made no progress" case this exists for), and advances it to `time(NULL)`
    only when a `write()`/`SSL_write()` call actually accepts bytes (`n > 0`). Getting `EAGAIN` does
    **not** advance it — that's what makes it measure stall time instead of merely "response still
    pending," and lets a response that's still slowly draining survive indefinitely (each accepted
    byte resets the clock) while one making zero progress at all gets caught.
  - The keep-alive branch resets `last_write_progress` back to `0` once a response is fully queued,
    same lifecycle as `request_started`.
  - `close_idle_connections`'s pending-write branch, which previously just `continue`d
    unconditionally, now closes the connection first if `now - last_write_progress >=
    WRITE_TIMEOUT_SECONDS` (with a comment: nothing left to say — the response was already mid-flight,
    so there's no 408-equivalent to send, just a reclaim).
- No `lib/tls.c` changes needed: `flush_connection` calls `conn_write`, which already dispatches to
  `tls_connection_write` for TLS connections, so the same clock covers both transports without
  touching the TLS module.

**Tests and results.**

- Unit tests added to `tests/test_connection.c` (registered in `main`):
  - Updated `test_close_idle_connections_skips_pending_write` to set `last_write_progress` to "now"
    (previously it only asserted pending writes were exempt outright; now it asserts a write that's
    actively progressing survives).
  - `test_close_idle_connections_write_stall_closes_connection` — a pending write with
    `last_write_progress` backdated past `WRITE_TIMEOUT_SECONDS` is reclaimed with nothing sent back.
  - `test_flush_connection_write_stall_reclaimed_by_close_idle_connections` — the strongest of the
    three: goes through the *real* `flush_connection` path (not a manually poked field) with an 8 MiB
    response over a `socketpair(2)` whose peer end is never read. `flush_connection` genuinely hits
    `EAGAIN` partway through (asserted via `conn->out_sent < conn->out_len`, proving this isn't a
    synthetic setup) and arms `last_write_progress` on its own. Backdating that timestamp and
    re-running the sweep then reclaims the connection — this is the exact "needs a response larger
    than the socket buffers" scenario `improvements.md` flagged as untested for S2, now covered.
- `make test`: all 14 suites pass (17 connection-suite cases now, up from 14).
- `make SANITIZE=1 BUILD_DIR=build-asan test`: all 14 suites pass clean under ASan + UBSan, including
  the 8 MiB-buffer test (the buffer is `malloc`'d directly rather than arena-owned, matching the
  pre-existing convention in this file's other manual `out_buf` fixtures, and is explicitly `free`'d
  after `app_destroy`).
- Compiled `lib/connection.c` directly with the project's `-Wall -Wextra -std=c11 -O2` flags: no new
  warnings.
- Live end-to-end reproduction was attempted against the demo server (a 20 MiB file under
  `/static/bigfile.bin`, a client connecting with a pinned small `SO_RCVBUF` and never calling `recv`
  for 35 s) but was **inconclusive**: macOS loopback auto-tunes socket buffer capacity well past what
  a request under `app_serve_static`'s `MAX_STATIC_FILE_SIZE` (50 MiB) cap can exhaust — the full
  20 MiB round-tripped in under a second regardless of the pinned receive-buffer hint, so the server
  never actually saw backpressure. This matches `improvements.md`'s own S2 entry, which was marked
  "Not tested (needs a response larger than the socket buffers)" for the identical reason. The
  `socketpair`-based unit test above is the authoritative verification here: Unix-domain socketpairs
  have much smaller, non-auto-tuned buffers, so an 8 MiB unread response reliably forces a real
  `EAGAIN`, unlike TCP loopback.

**Status:** Fixed. `WRITE_TIMEOUT_SECONDS` is a compile-time constant in `lib/app_types.h`, same
convention as S1's deadlines.

---

## S3 · No connection limit, no overload behavior

**Date completed.** 2026-09-22.

**How it was completed.**

`accept_connections` used to be bounded only by `RLIMIT_NOFILE`: past it, `accept()` starts failing
and the worker just stops accepting, with no cap of its own and no way to tell a client the server is
full. Two independent mechanisms were added, matching `improvements.md`'s fix list items (1) and (2);
(3) (an optional per-IP cap) was deliberately skipped to keep this an S-effort fix — noted below.

- `lib/app_types.h`: added `ServerConfig.max_connections` (default `DEFAULT_MAX_CONNECTIONS` = 10,000,
  applied by `app_init`; `0` is a deliberate opt-out, uncapped) and two `App` fields:
  `open_connections` (a running count) and `spare_fd` (one reserved descriptor).
- `lib/router.c`: `app_init` sets `config.max_connections` to the default and opens `spare_fd`
  (`open("/dev/null", O_RDONLY)`; best-effort, `-1` on failure just means the EMFILE mechanism below
  degrades to the pre-S3 behavior rather than the whole server failing to start).
- `lib/connection.c`:
  - **The cap.** `accept_connections` checks `open_connections >= max_connections` right after a
    successful `accept()`, before `ensure_connection_capacity`/`connection_create` — so an overloaded
    worker doesn't pay for either. Past the cap, a new `reject_overloaded_connection` helper writes a
    hand-built `503 Service Unavailable` + `Connection: close` (no `Connection`/arena/`Response` — none
    of that machinery exists yet at this point) in one best-effort nonblocking write, then closes the
    fd. It skips the plaintext write entirely when `app->ssl_ctx != NULL`: a TLS listener's client
    expects a handshake, not HTTP text, so writing anything would just look like protocol garbage
    instead of a 503.
  - `open_connections` is a running counter (`++` in `accept_connections`, `--` in `connection_close`),
    not a rescan of `app->connections` — the existing `app_count_connections` (O(n)) was deliberately
    left alone (still used at shutdown) rather than reused here, since calling an O(n) scan once per
    *accepted* connection would turn a connection flood into the same O(n)-per-connection problem this
    check exists to prevent.
  - **The EMFILE fallback.** On `accept()` failing with `EMFILE`/`ENFILE`, `spare_fd` is closed (freeing
    one descriptor) and `accept_connections` retries. See the MEASURED finding below — this needed a
    full redesign partway through once real testing showed the naive version didn't work.
  - `create_server_socket`'s `listen()` call now uses `max(BACKLOG, SOMAXCONN)` instead of the bare
    `BACKLOG` (128) constant — a no-op on macOS (`SOMAXCONN` is also 128 there) but widens the pending-
    accept queue on a Linux whose `SOMAXCONN` is raised, per `improvements.md`'s note.
  - `app_destroy` closes `spare_fd`.

**MEASURED finding that changed the design.** The first implementation of the EMFILE fallback followed
`improvements.md`'s fix literally: on `EMFILE`, close `spare_fd`, retry `accept()` for *that* connection,
answer it with a 503, then reopen `spare_fd`. Testing it against a real tightened `RLIMIT_NOFILE`
showed the retry reliably returned `EAGAIN`, not the connection — on this machine (macOS), `accept()`
does not leave a completed connection in the listen backlog for a retry when it fails to allocate an
fd for it; it dequeues and destroys that connection as part of failing, rather than leaving it queued.
So there is nobody left to send a 503 to by the time the failure is observed. (Not verified on Linux;
a kernel that instead leaves it queued would make the retry harmless and possibly even recover it, but
nothing here relies on that.) The fix was redesigned around what's actually achievable: `spare_fd` is
freed and *not* immediately reopened, so the *next* `accept()` call — for whatever connection comes
after the lost one, now or on a later call to `accept_connections` — succeeds instead of the worker
staying stuck at the limit indefinitely. `connection_close` opportunistically re-arms `spare_fd` the
moment anything closes, rather than waiting for another `accept_connections` call that might not come
for a while if the listen socket itself is the thing that's starved.

Deliberately *not* done: (3) a per-IP cap (explicitly "optional" in `improvements.md`'s fix list) —
`max_connections` and the `EMFILE` fallback address the memory-bound and total-fd-exhaustion cases;
a single misbehaving IP hogging a large fraction of the cap is a real but separate problem, left for
later. Also not done: making `max_connections` a per-IP-aware or dynamically adjustable value, or
exposing it as an env var in the `examples/todo_sqlite` demo (it's set directly on `app.config`, the
same pattern already used for `port`/`workers`/etc. in application code).

**Tests and results.**

- Unit tests added to `tests/test_connection.c` (registered in `main`), needing a real loopback TCP
  listener rather than `socketpair(2)` (new helpers `setup_test_server`, `connect_loopback_client`,
  `drain_accept_connections`):
  - `test_accept_connections_enforces_max_connections` — 3 real concurrent client connections against
    `max_connections = 2`: the first two are accepted, the third gets an explicit 503 body with
    `Connection: close`.
  - `test_accept_connections_max_connections_zero_is_unlimited` — 5 concurrent connections against
    `max_connections = 0` all succeed (the opt-out sentinel isn't mistaken for "cap of zero").
  - `test_accept_connections_emfile_frees_a_slot_and_recovers` — pins `RLIMIT_NOFILE` (via `getrlimit`/
    `setrlimit`, restored before any assertion can bail the test binary) to exactly the descriptor count
    already in use, forcing a real `EMFILE` from `accept()`. Matching the MEASURED finding above, it does
    **not** assert a 503 for the connection that triggered the failure (asserts only that it's closed,
    `n <= 0`); it asserts the actual guarantee: a later connection is accepted normally once the retry
    frees a slot, and `connection_close` re-arms `spare_fd`.
  - `drain_accept_connections` retries `accept_connections` a few times with short pauses rather than
    calling it once: a rare race (a `connect()` that has returned client-side but isn't yet visible to
    the server's `accept()`) surfaced intermittently under the ASan build during development — a 15%
    failure rate across 20 runs — and was fixed by this retry rather than by loosening any assertion.
    Confirmed clean over 30 consecutive ASan runs afterward.
- `make test`: all 14 suites pass, plain and stress-run repeatedly (15 runs).
- `make SANITIZE=1 BUILD_DIR=build-asan test`: all 14 suites pass clean under ASan + UBSan; the new
  EMFILE test specifically stress-run 30 times with zero failures after the `drain_accept_connections`
  fix (see above).
- Compiled `lib/connection.c` and `lib/router.c` directly with the project's `-Wall -Wextra -std=c11
  -O2` flags: no new warnings (the pre-existing `router.c` warnings on `next_seg_len`/`seg_len`/
  `strncpy` are unrelated to this change and were present before it).
- Live end-to-end verification against a minimal standalone server (not the `examples/todo_sqlite`
  demo, to avoid adding a `MAX_CONNECTIONS` env var to it for a one-off test) built against the real
  library with `app.config.max_connections = 5`: 15 concurrent clients produced exactly 5 `200 OK`
  responses and 9 explicit `503 Service Unavailable` responses (one client saw a connection reset, a
  benign client-side race with the server closing right after writing); a liveness check immediately
  afterward still returned `200 OK`, confirming the server both degrades gracefully under the cap and
  stays fully responsive once the flood subsides.
- Live end-to-end reproduction of the exact scenario `improvements.md` measured for this item
  (`ulimit -n 40`, then ~100 concurrent clients, against `examples/todo_sqlite`'s demo) confirmed the
  same shape reported there — the first ~32 connections got a real response and the rest got no data
  at the TCP level (consistent with the MEASURED macOS `accept()`/`EMFILE` finding above: those
  connections' completed handshakes are destroyed by the kernel before the server can answer them) —
  and additionally confirmed a liveness check immediately after the flood still got `200 OK`, i.e. the
  worker was not left permanently stuck the way `improvements.md` described ("stayed idle... never
  told anyone it was full").

**Status:** Fixed, with one caveat carried forward rather than hidden: the `EMFILE` fallback cannot
deliver a 503 for the specific connection that exhausts the descriptor table on the kernels tested here
(MEASURED, macOS) — only the `max_connections` cap (checked well before the OS limit, by default at
10,000) delivers the clean "accept, answer 503, close" behavior `improvements.md` asked for. Anyone
relying on graceful 503s under true fd exhaustion, rather than under the configured cap, should treat
that as a known limitation, not a guarantee.

---

## S4 · A declared `Content-Length` reserves 10 MiB per connection immediately

**Date completed.** 2026-09-22.

**How it was completed.**

`improvements.md`'s fix had two parts — grow the input buffer geometrically as bytes actually arrive
(as chunked already did), and add a way to reject an oversized declared body earlier than the global
10 MiB cap, per route/prefix. Both were implemented.

- **Geometric growth for `Content-Length` (`lib/connection.c`: `grow_in_buf`).** Previously, once
  `in_buf` filled and headers were complete, a declared `Content-Length` reallocated `in_buf` in one
  step straight to `header_len + content_length + 1` — trusting the client's declared size regardless
  of how many body bytes had actually shown up. Rewrote that branch to mirror the chunked branch
  already next to it: double `in_cap` each time (capped at the same target size) instead of jumping
  straight there, so the reservation grows in step with data genuinely received. A client that
  declares a 10 MiB body and stalls after 9 KB now costs a few doublings from `BUF_SIZE` (8 KiB), not
  a single ~10 MiB allocation. Added a defensive early-return (`if (conn->in_cap >= target) return 1`)
  mirroring chunked's analogous cap-hit guard, for the case where a prior growth step already reached
  the target exactly.
- **Per-route/prefix body limits (`lib/router.c`/`.h`, `lib/app_types.h`).** New `app_use_body_limit(App
  *, prefix, max_bytes)`, matching `app_use_prefix`'s segment-boundary prefix semantics (its own small
  `body_limit_prefix_matches` static, not shared with `middleware.c`'s copy — the lookup runs before a
  `Request` exists, so there's nothing to hand a `Request`-shaped helper). Registrations go into
  `App.body_limits[MAX_BODY_LIMITS]` (16 slots, excess dropped with a stderr warning, same convention
  as `app_use_prefix`/`MAX_MIDDLEWARES`); `max_bytes` above `MAX_BODY_SIZE` is clamped down with a
  warning — a route can only *tighten* the global cap, never loosen it, so nothing here can be used to
  accept more than `MAX_BODY_SIZE` ever could. `app_body_limit_for_path` returns the *longest* matching
  prefix's limit (specificity beats registration order, same rule routing itself uses), or
  `MAX_BODY_SIZE` if nothing matches — so an app that never calls `app_use_body_limit` sees no behavior
  change at all.
- **Early rejection (`lib/connection.c`: `reject_if_over_body_limit`, called from `handle_readable`
  right after every `recv`, before `request_is_complete`).** This is the part that needed
  `request_framing` to expose the request path: added optional `path_out`/`path_len_out` parameters
  (`lib/http_parser.h`/`.c`; `NULL` skips them, and every pre-existing caller — `request_is_complete`,
  `parse_http_request`, `close_idle_connections`'s sweep-time framing check, the buffer-full-branch
  check in `handle_readable` — was updated to pass `NULL, NULL` and is otherwise unchanged). As soon as
  headers are complete, `reject_if_over_body_limit` looks up the effective limit for the request's raw
  (not percent-decoded) path and, if the declared `Content-Length` exceeds it, sends 413 immediately —
  before `grow_in_buf` ever runs, before a single body byte is buffered. A new `Connection.body_limit_checked`
  flag (zero-initialized by the existing `calloc`, cleared alongside `request_started`/`last_write_progress`
  in `flush_connection`'s keep-alive branch) makes this run at most once per request rather than once per
  `recv` while a body is still trickling in.

**Deliberately scoped down**, matching the S1–S3 precedent of narrowing rather than silently doing
less than advertised:
- **`app_use_body_limit` covers `Content-Length` only, not chunked.** Threading a per-route limit into
  `chunked_body_scan`/`grow_in_buf`'s chunked branch would mean changing `request_is_complete` and
  `parse_http_request`'s signatures (both take only `buf`/`len`, by design — "HTTP parsing functions
  should be pure ... take `const char*` buffers", `CLAUDE.md`) to also take an app-supplied limit,
  which ripples into every caller in `tests/` and `tests/fuzz_parser.c`. `improvements.md`'s own
  MEASURED problem statement for S4 is specifically about `Content-Length`'s one-shot reservation;
  chunked's raw-wire cap was already doubling-and-capped before this fix, so it doesn't share the
  measured problem. Chunked bodies remain governed by the single global `MAX_BODY_SIZE` only,
  regardless of `app_use_body_limit` registrations for their path.
- **Path matching for body limits uses the raw (non-percent-decoded) request-target**, not the decoded
  `req->path` that route matching and app-wide middleware use — there is no parsed `Request` yet at
  the point this check has to run (that's the whole reason it exists: to reject before the body,
  and therefore before a full parse, happens). A prefix containing percent-encoded characters
  (`%2F` etc.) will not match the way it would against a decoded path. Not expected to matter in
  practice (`app_use_body_limit` prefixes are written by the application, like `app_use_prefix`'s
  already are, and are realistically plain ASCII paths), but noted rather than silently assumed away.
- `MAX_BODY_LIMITS` (16) is a fixed array, like `MAX_MIDDLEWARES`, not a dynamically grown list — an
  app registering more than 16 prefixes needs to consolidate, or the doc's own "excess is dropped, not
  overflowed" convention (`CLAUDE.md`, "Limits") applies.

**Tests and results.**

- Unit tests added to `tests/test_router.c` (registered in `main`): `test_body_limit_defaults_to_max_body_size`
  (no registrations → `MAX_BODY_SIZE` everywhere), `test_body_limit_prefix_scoping` (segment-boundary match:
  `/api/uploads` matches itself and `/api/uploads/avatar`, not `/api/uploadsx`), `test_body_limit_longest_prefix_wins`
  (registered broad-then-narrow, deliberately out of specificity order — narrower still wins, mirroring
  routing's own "specificity beats registration order"), `test_body_limit_unscoped_prefix_applies_everywhere`
  (`""` and `NULL` both behave as unscoped), `test_body_limit_clamped_to_max_body_size` (a limit requested
  above `MAX_BODY_SIZE` is clamped down, confirmed via the return value, not just the stderr warning),
  `test_body_limit_overflow_is_dropped` (the 17th registration is dropped, `body_limit_count` stays at
  `MAX_BODY_LIMITS`).
- Unit tests added to `tests/test_http_hardening.c`: `test_request_framing_path_out` — a complete request
  returns the exact raw path and length pointing into the caller's own buffer; an incomplete request
  (`header_len == 0`) doesn't crash when `path_out`/`path_len_out` are requested but unavailable. Every
  pre-existing `request_framing` call site in this file (4 call sites) updated to pass `NULL, NULL`.
- Unit tests added to `tests/test_connection.c` (registered in `main`):
  - `test_handle_readable_content_length_grows_geometrically` — a 5 MiB declared body fed in 4 KiB
    writes (matching the existing large-body test's pattern, since a nonblocking `socketpair` fd can't
    be counted on to accept a large write in one call without a drain in between); the first time
    `conn->in_cap` grows past `BUF_SIZE` (only a few KB genuinely received so far), it asserts `in_cap`
    is nowhere close to the 5 MiB declared (`< declared_len / 4`) — the exact behavior the old one-shot
    realloc violated. The request is then completed normally to a 200, confirming the geometric growth
    doesn't break large legitimate bodies, just how gradually they're paid for.
  - `test_handle_readable_route_body_limit_413` — a route under an `app_use_body_limit`-covered prefix
    with a declared `Content-Length` over that limit (but comfortably under the global `MAX_BODY_SIZE`,
    so this is specifically exercising the new per-route path, not the pre-existing global one) gets
    413 immediately and the connection is closed.
  - `test_handle_readable_route_body_limit_allows_within_limit` — same route and limit, a body within
    it completes normally to a 200 with the correct byte count, confirming the check doesn't
    false-positive on ordinary requests.
- `make test`: all 14 suites pass (test_router: 6 new cases; test_http_hardening: 1 new case, plus 4
  existing `request_framing` call sites updated; test_connection: 3 new cases).
- `make SANITIZE=1 BUILD_DIR=build-asan test`: all 14 suites pass clean under ASan + UBSan.
- `make fuzz` (200,000 iterations): clean. `fuzz_parser.c`'s `request_framing` call updated to exercise
  the new `path_out`/`path_len_out` params (previously fuzzed with `NULL, NULL`) rather than leave the
  new code path outside the fuzzer's reach.
- `make check-docs`: passes (130 engine functions covered) — `app_use_body_limit` and
  `app_body_limit_for_path` documented in `lib/API.md`, `request_framing`'s entry updated for the two
  new optional parameters.
- Compiled `lib/connection.c`, `lib/router.c`, `lib/http_parser.c` directly with the project's
  `-Wall -Wextra -std=c11 -O2` flags: no new warnings (the pre-existing `router.c` warnings on
  `next_seg_len`/`seg_len`/`strncpy` predate this change, noted in `lib/CLAUDE.md`, "Hot-path rules").
  Fixed one warning of our own making (`-Wcomment`: a doc-comment line containing `/*path_len_out`
  triggered "'/*' within comment").
- Live end-to-end reproduction against a minimal standalone server built with the real library
  (`app_use_body_limit(&app, "/limited", 1024)`): a raw-socket request declaring `Content-Length: 2000`
  against `/limited` got `413 Payload Too Large` in under 0.1 ms — before any body bytes were sent —
  while the same 2000-byte body against an unlimited route (`/upload`) completed normally with `200 OK`.
- **Live reproduction of the exact MEASURED scenario from `improvements.md`** (300 connections, each
  declaring a 10 MiB `Content-Length` and sending 9 KB of body): `improvements.md` reported **+3,000 MB
  virtual** for this. After the fix, `ps -o vsz` on the same standalone server showed **0 measurable VSZ
  growth** (435,304,880 KB before and after — identical), with RSS growing by ~9.5 MB (proportional to
  the ~9 KB × 300 connections actually sent, consistent with each connection's `in_buf` doubling only as
  far as needed to hold what arrived, not the declared 10 MiB).

**Status:** Fixed for the measured problem (`Content-Length` reservation now proportional to data
received, not declared) and for the "reject early" half of the fix (per-route/prefix limits, checked
before any buffering). Chunked bodies and non-ASCII-prefix matching are explicitly out of scope, per
the narrowing above — not gaps introduced by this work, but existing global behavior this change did
not touch.
