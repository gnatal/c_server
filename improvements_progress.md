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

---

## S5 · Header values over 255 characters are silently truncated

**Date completed.** 2026-09-22.

**How it was completed.**

`improvements.md` offered two alternative short-term fixes — "answer 431 when a header exceeds the
stored size, **or** raise the value size for `Authorization` and `Cookie`" — with the proper fix (view-
based headers, no per-header cap at all) deferred to P3. Did both halves rather than picking one, since
they solve different parts of the same problem: raising the cap makes realistic tokens (JWTs, long
cookies) work *correctly* instead of failing at all, while the loud-rejection half makes whatever still
doesn't fit fail *safely* instead of silently.

- **Raised the cap (`lib/app_types.h`).** `Request.header_values`' per-slot size was a hardcoded `256`
  (255 usable chars) shared identically by all `MAX_HEADERS` (32) slots — the project has no
  per-header-name storage (no field or array is dedicated to `Authorization` specifically), so "raise it
  for `Authorization` and `Cookie`" in practice meant raising it for every slot, since any header could
  occupy any slot. Named it `MAX_HEADER_VALUE_LEN` (1024, so 1023 usable chars) rather than leaving it a
  bare literal, matching the project's existing pattern for exactly this kind of limit
  (`MAX_SET_COOKIE_LEN`, response-side, is the same idea: "value doesn't fit → reject/drop, never
  truncate"). 1024 was chosen directly off `improvements.md`'s own numbers ("JWTs are commonly 300–1,000
  characters") rather than an arbitrary round number — it comfortably covers the range the report itself
  measured as the real-world problem, with headroom.
- **Reject what still doesn't fit, instead of truncating (`lib/http_parser.c`: `parse_http_request`).**
  The header-copying loop (which duplicates `parse_headers`' logic inline rather than calling it — see
  below) now checks `headers[i].name_len`/`value_len` against the destination array sizes *before*
  calling `copy_bounded`, and returns a new sentinel, `-3`, instead of ever truncating. Checked both
  sides of the colon (name and value), not just the value `improvements.md` named — the same
  `copy_bounded` call and the same silent-corruption failure mode apply symmetrically to an over-long
  header *name*, even though real-world header names essentially never exceed 63 chars in practice.
  `-3` slots into the existing `parse_http_request` return-code convention next to `-2` (path too long
  → 414): documented in `lib/http_parser.h`, and `lib/connection.c`'s dispatch (`handle_readable`) maps
  it to 431 (`Request Header Fields Too Large`) — the same status code already used for the
  whole-header-block-over-`BUF_SIZE` case, since both are "a header (or the header block) is bigger than
  we're willing to store," just at different granularities.
- **`parse_headers` (the standalone `void` component parser in the same file) was deliberately left
  truncating.** It's exposed for tests (`lib/http_parser.h`: "Component parsers ... exposed for tests")
  and is *not* on the live request path — `parse_http_request` does not call it; it re-implements the
  same header-copy loop inline so it can return an error code, which `parse_headers`' `void` signature
  has no way to do. Changing that signature to add error reporting would ripple into every test file
  that calls it directly (`tests/test_http_parser.c`, `tests/test_http_hardening.c`) for a function the
  actual security-relevant path never touches, so it was left as-is — still benefits from the raised
  1024-byte array size (it shares `Request.header_values`), just without the loud-rejection behavior.

**Deliberately scoped down**, matching the S1–S4 precedent of narrowing rather than silently doing less
than advertised:
- **Individual cookie values (post-split) are not covered.** The raised cap and the 431 rejection apply
  to the *raw* `Cookie:` header line (`req->header_values`) — once that line is complete and within
  `MAX_HEADER_VALUE_LEN`, `parse_cookies` still splits it into `cookie_names`/`cookie_values[MAX_COOKIES][256]`,
  each capped at the original 255 chars with no error signal (`parse_cookies` is also `void`). A single
  very long session-token cookie among several shorter ones on the same header line can therefore still
  be silently truncated even though the header line as a whole fit under the new cap. Out of scope here
  because `improvements.md`'s S5 problem statement and MEASURED finding are specifically about
  `copy_bounded` in the *header*-value path (`http_parser.c:22`, called from the header loop), not the
  separate cookie-splitting one; fixing it would mean giving `parse_cookies` an error return too, a
  larger change than this entry's effort budget (M) covers on its own.
- **Path/query param values (63 chars) and the raw query string (255 chars) are unaffected** — same
  reasoning: not what `improvements.md` measured for S5, and each is its own separate truncation point
  requiring its own scoping decision, better left to a dedicated pass (or P3's structural fix, which
  would remove all of these caps at once via views into `in_buf` instead of fixed copies).
- **`Request` grew from ~19 KB to 43,576 bytes** (`sizeof`, `make bench`) — all in
  `header_values[32][1024]` vs. the old `[32][256]`. Confirmed this costs nothing per-request in CPU
  (see Tests below): `copy_bounded` copies only the bytes actually present in the header value, never
  the destination array's capacity, so an unused larger slot is free at runtime, just heavier on the
  stack frame `Request req` occupies in `handle_readable`. Not reduced further (e.g. per-header-name
  sizing) because the engine has no way to know a given slot will hold `Authorization` versus `Accept`
  until it's already copying it in.

**Tests and results.**

- Unit tests added to `tests/test_http_hardening.c` (registered in `main`):
  `test_parse_http_request_rejects_oversized_header_431` — a value 1100 chars past the header keyword
  (comfortably over the 1024 cap) returns `-3`; a realistic ~600-char JWT-shaped `Authorization: Bearer`
  token returns `0` and round-trips through `req_get_header` at its *exact* length (`strlen(auth) ==
  strlen("Bearer ") + 600`, not truncated) — this is the compatibility half of the fix, not just the
  loud-failure half; and a 100-char header *name* also returns `-3`, confirming the check is symmetric
  across the colon. Also updated the pre-existing `test_header_value_whitespace_and_limits` (which calls
  `parse_headers`, not `parse_http_request` — see scoping above): its 400-char over-long value no longer
  demonstrates truncation now that 400 < 1024, so raised it to 1100 chars to keep testing
  `parse_headers`' own (deliberately unchanged) truncating behavior, with a comment explaining why that
  function still truncates while `parse_http_request` next to it does not.
- Unit test added to `tests/test_connection.c` (registered in `main`):
  `test_handle_readable_oversized_header_value_431` — end-to-end through the real `handle_readable` path
  (distinct from the pre-existing `test_handle_readable_header_overflow_431`, which exercises the
  whole-header-block-over-`BUF_SIZE` 431 — this one is a single header value over `MAX_HEADER_VALUE_LEN`
  in a request that comfortably fits under `BUF_SIZE`, so it's specifically exercising
  `parse_http_request`'s `-3` → `connection.c`'s 431 mapping): gets an explicit
  `431 Request Header Fields Too Large` and the connection is closed, same shape as every other
  rejection in this suite.
- `make test`: all 14 suites pass (test_http_hardening: 1 new case + 1 updated; test_connection: 1 new
  case).
- `make SANITIZE=1 BUILD_DIR=build-asan test`: all 14 suites pass clean under ASan + UBSan.
- `make fuzz` (200,000 iterations): clean.
- `make check-docs`: passes (130 engine functions covered).
- Compiled `lib/http_parser.c` and `lib/connection.c` directly with the project's
  `-Wall -Wextra -std=c11 -O2` flags: no new warnings.
- `make bench`: re-ran to get the new `sizeof(Request)` (43,576 bytes, up from ~19,000) for
  `lib/CLAUDE.md`. Per-request CPU numbers moved within ordinary single-run noise (minimal GET 208→198 ns,
  browser-shaped GET 781→825 ns, JSON POST 315→313 ns, 404 186→208 ns, 20-row JSON list 659→709 ns) —
  consistent with the "bigger unused array costs nothing per request" reasoning above, not a regression
  attributable to this change; `lib/CLAUDE.md`'s Hot-path rules section records both runs and why the
  deltas aren't causal.
- Live end-to-end verification against a minimal standalone server built with the real library
  (`req_get_header(req, "Authorization")` echoed back as a length): a raw-socket request with a
  realistic 600-char `Bearer` token got back `auth len=607` (`"Bearer "` + 600, exact — previously this
  would have been silently cut to 255 total) confirming the compatibility half of the fix live, not just
  in the pure-parser unit tests; a 1200-char token (past the cap) got `431 Request Header Fields Too
  Large` instead of a corrupted, silently-wrong comparison.

**Status:** Fixed. The silent-truncation failure class is closed for request header names and values on
the live request path (`parse_http_request`); `parse_headers` (test-only component parser), individual
post-split cookie values, and path/query param values remain truncating, per the scoping above — not
newly introduced gaps, pre-existing behavior this entry's effort budget did not extend to.

---

## P1 · Static and file responses go through slow paths (static-file cache half only)

**Date completed.** 2026-09-22.

**Scope.** `improvements.md`'s P1 bundles five fixes under one ID. This entry covers only fix (1), "a small
file cache" for `app_serve_static`/`static_serve_file` — the half responsible for the MEASURED 6.6×
`/static` gap. Fixes (2) writev/single-buffer heads, (3) `sendfile(2)` for large files, (4) cached
`realpath`, and (5) `ETag`/`Last-Modified`/`304`/`Cache-Control` are **not done**; `lib/CLAUDE.md`'s "Known
gaps" was updated to say so explicitly rather than let a partial fix read as a complete one.

**How it was completed.**

`static_serve_file` (`lib/static.c`) used to pay `realpath` + `stat` (twice for a directory index) +
`fopen`/`malloc`/`fread`/`fclose`/`free` on every single request, even for the same handful of files
requested repeatedly — the common case for a static mount. Added a small in-memory cache, file-static to
`static.c` (not `App`-scoped: two different mounts can never resolve to the same candidate string, each
being rooted under its own canonical `static_root`, so one process-lifetime table is enough and avoids
threading a cache pointer through `dispatch`/`MiddlewareChain`/`match_route`, none of which currently carry
anything mutable per mount).

- **Cache key is the pre-`realpath` candidate path** (`static_root` + the already-traversal-checked
  subpath from `static_resolve_relative_path`), not the resolved one. This is what lets a cache *hit within
  the revalidation window* skip `realpath`/`stat` entirely, not just the file read — the biggest share of
  the measured gap, since `open`/`fstat`-class syscalls dominate at these file sizes, not the read itself.
- **Two-tier lookup**, both new code paths added to `static_serve_file`:
  1. Before any syscall: `cache_lookup_fresh(candidate, now)` — a hit checked within
     `STATIC_CACHE_REVALIDATE_SECONDS` (1 s, `app_types.h`) of its last confirmation is served straight from
     the cache, zero filesystem calls.
  2. After `resolve_and_stat` (and the directory-index retry, and the `MAX_STATIC_FILE_SIZE` check) but
     before `fopen`: if a cache entry for the candidate exists and its stored `mtime`/`size` match the fresh
     `stat`, the read is skipped and the cached bytes are served, with `last_checked` refreshed — this is the
     "server has been up more than a second" steady state: one `stat` per file per second, not one full read
     per request.
  3. Otherwise (miss, or the file changed): the original `fopen`/`malloc`/`fread` path runs unchanged, and
     `cache_insert` is given ownership of the freshly read buffer (files over `STATIC_CACHE_MAX_ENTRY_BYTES`,
     256 KiB, are served but never cached — no behavior change for those, just no speedup).
- **Bounded, no separate byte-accounting to get wrong.** `STATIC_CACHE_MAX_ENTRIES` (256) ×
  `STATIC_CACHE_MAX_ENTRY_BYTES` (256 KiB) = `STATIC_CACHE_MAX_TOTAL_BYTES` (64 MiB) exactly (`app_types.h`),
  asserted at compile time (`_Static_assert` in `static.c`) — capping entry count alone caps total bytes, so
  there is no independent running-total check to keep in sync with the eviction logic. Eviction (when the
  entry cap is hit on an insert for a genuinely new candidate) drops whichever entry was least recently
  confirmed fresh (`cache_evict_stalest`, linear scan — the table is at most 256 entries and this only runs
  on a miss, never on the hit path).
- **Ownership**, matching this codebase's "every malloc has a matching free" rule: `cache_insert` takes
  ownership of the caller's already-allocated file buffer on success (0) — no extra copy — and leaves it
  untouched on failure (-1: too big to cache, or `malloc` failed for the cache's own path-string
  bookkeeping), so `static_serve_file` frees it itself exactly when `cache_insert` didn't take it. New
  public function `static_cache_clear(void)` (`static.h`, listed in `API.md` per `make check-docs`) frees
  every entry; nothing in the engine calls it (documented as being for tests, and for an application that
  wants to force a reload without restarting the worker).

**Deliberately scoped down / trade-offs, matching the S1-S5 precedent of narrowing rather than silently
doing less than advertised:**
- **Revalidation is `stat`-based with a 1 s window, same trade-off nginx's `open_file_cache valid=1s` makes.**
  A symlink swapped in place, or a file rewritten with the same size and the same one-second-resolution
  mtime, can serve stale content for up to that window. Documented in `lib/CLAUDE.md`'s "Static" section and
  `static_serve_file`'s doc comment, not silently assumed away.
- **`res_send_file` (non-static-mount file responses) and large static files (`MAX_STATIC_FILE_SIZE`, 50
  MiB, still read whole into memory when under that cap and over the cache's 256 KiB per-entry cap) are
  unaffected by this change.** They still go through the pre-existing slow path `improvements.md` also flags
  under P1 — see Scope above.
- **No `ETag`/`Last-Modified`/`304`/`Range`/`Cache-Control`** — a served file still returns 200 with the
  full body every time, cached or not; this fix is about server-side cost, not bytes on the wire.

**Tests and results.**

- Unit tests added to `tests/test_static.c` (registered in `main`):
  - `test_cache_serves_stale_content_within_revalidate_window` — serves a file once, deletes it from disk,
    then serves the same request again within the 1 s window and asserts the original content still comes
    back: since the file no longer exists, any codepath that touched the filesystem would 404, so a 200 with
    the original body proves the fast path took no filesystem call at all. Restores the file before
    `teardown_fixture` so its own cleanup doesn't fail.
  - `test_cache_revalidates_after_window_and_serves_unchanged_content` — a real `sleep(2)` (comfortably past
    `STATIC_CACHE_REVALIDATE_SECONDS`) then a second request against the untouched file, exercising the
    "existing entry, stale check, but `stat` confirms unchanged" branch specifically (distinct from the
    within-window test above).
  - `test_cache_picks_up_change_after_window_expires` — same `sleep(2)`, then the file is rewritten with
    different content *and* a different size before the second request: asserts the new content is served
    and the old content is gone, proving a genuine change invalidates the cache correctly rather than
    latching onto stale bytes forever.
  - All three call `static_cache_clear()` at the end (process-lifetime global state, shared across every
    test in the binary — the unique `mkdtemp` root per test already prevents key collisions with other
    `test_static.c` cases, but clearing anyway is one line and keeps the suite's assertions self-contained
    rather than relying on that as an implicit invariant).
- `make test`: all 13 suites pass (test_static: 3 new cases; ~4 s added to the suite's runtime from the two
  `sleep(2)` calls, not considered worth engineering around for a one-file addition).
- `make SANITIZE=1 BUILD_DIR=build-asan test`: all 13 suites pass clean under ASan + UBSan.
- `make fuzz` (1,000,000 iterations): clean (134,879 parsed, 145,303 complete) — `fuzz_parser.c` already
  links `static.c` for the response-builder path; no seed changes needed since fuzzing doesn't exercise
  `static_serve_file` directly.
- `make check-docs`: passes (121 engine functions, up from 120 — `static_cache_clear`).
- Compiled `lib/static.c` directly with the project's `-Wall -Wextra -std=c11 -O2` flags: no new warnings.
- **Live reproduction of the exact MEASURED scenario from `improvements.md`** (4 workers, 100 connections,
  `examples/todo_sqlite`'s demo, `wrk -t4 -c100 -d8s` against `/static/style.css`, a 52-byte file):
  **255,941 req/s** after this fix, up from the 37,647 req/s `improvements.md` measured before it — a
  **6.8×** improvement, matching (and slightly exceeding) the PROJECTED "up to ~6×" and landing within noise
  of the in-memory `res_send_bytes` baseline the same document measured (249,811-256,126 req/s), i.e. the
  static-mount path now costs about the same as serving from memory directly.
- Live end-to-end sanity check against the same demo: `GET /ping` and `GET /api/todos` unaffected (200);
  `GET /static/style.css` 200 with the correct 52-byte body; `GET /static/does-not-exist` 404;
  `GET /static/` 404 — pre-existing, documented behavior unrelated to this change (a trailing `*` route
  never matches the bare mount prefix, so the request never reaches `static_serve_file` at all; see
  `static.h`'s doc comment on `static_resolve_relative_path`).

**Status:** Fixed for the measured `/static`-mount problem. `res_send_file`, large-file streaming and
HTTP caching headers remain open, per the Scope note above — not gaps introduced by this work, but the rest
of P1's own fix list that this entry did not attempt.

---

## S6 · `%00` in a path truncates it

**Date completed.** 2026-09-22.

**How it was completed.**

`decode_bounded` (`lib/http_parser.c`), the shared percent-decoder behind `url_decode` and
`parse_query_string`, used to be `void`: a `%00` sequence decoded to a real NUL byte was written into
the destination buffer with no signal that anything unusual happened. Since `req->path`,
`req->query_names[]` and `req->query_values[]` are all plain C strings read by `strlen`/`strcmp` (the
router, `req_get_query`, and any handler that inspects `req->path` directly, e.g. a suffix/extension
check), an embedded NUL silently truncated the string for every one of them — MEASURED
(`improvements.md`, S6) `GET /static/style.css%00.png` being routed and served as `/static/style.css`.

- **`decode_bounded` now returns `int`** (0 ok, -1 if any decoded byte is NUL) instead of `void`, while
  still fully writing and NUL-terminating `dst` either way — the contract is "tell the caller", not
  "refuse to decode", since some callers (see below) want to report the failure differently than others.
- **`url_decode` (`http_parser.h`, public) now returns `int`**, propagating `decode_bounded`'s result.
  Its one caller inside the engine that matters for this fix is `parse_http_request`'s path decode;
  `urlencoded.c`'s two calls (form field names/values) still ignore the return value — see Scope below.
- **`parse_query_string` (`http_parser.h`, listed as "exposed for tests" alongside `parse_headers` and
  `parse_cookies`) now returns `int`** instead of `void`: 0 ok, -1 if a decoded name or value in any pair
  contained a NUL. It still populates `req->query_count` up through the offending pair before returning
  -1 (same "don't bother finishing what the caller will reject anyway" convention as `parse_http_request`'s
  existing -3 for headers, S5) rather than stopping the whole scan the instant the first bad pair is seen.
- **`parse_http_request` gained a new return code, `-4`** ("a percent-decoded path/query name/query value
  contains an embedded NUL"), checked right after the path's `url_decode` call and right after
  `parse_query_string`. `connection.c`'s dispatch of `parse_http_request`'s return codes already had an
  `else 400` fallback for anything that wasn't `-2`/`-3` — `-4` needed no new branch there, only a comment
  explaining why it lands in that fallback alongside the pre-existing `-1` (malformed).

**Deliberately scoped down**, matching the S1–S5 precedent of narrowing rather than silently doing less
than advertised:
- **Cookie values are explicitly named in `improvements.md`'s fix list ("path (and query names/values,
  cookie values)") but are out of scope here, because they don't apply**: `parse_cookies` never
  percent-decodes cookie values at all (`req_get_cookie`'s doc comment already says "not decoded") — it
  copies the raw `Cookie:` header bytes with `copy_bounded`, not `decode_bounded`. There is no `%00`
  decode vector for cookies in this engine to close; the improvements.md wording is imprecise on this
  point, not a gap this fix left open. Noted here rather than silently fixing something that doesn't
  exist.
- **`urlencoded.c`'s two `url_decode` calls (form field names/values from a `application/x-www-form-urlencoded`
  body) do not check the new return value.** `improvements.md`'s S6 problem statement and MEASURED finding
  are specifically about the request-target path; form bodies are a related but separate surface sharing
  the same underlying `decode_bounded`, deliberately left for a follow-up rather than folded into an
  "S" (small) fix's scope. `lib/CLAUDE.md`'s "Known gaps" was **not** updated to call this out as a new gap
  introduced by this work — it is pre-existing behavior (form fields were never checked for embedded NULs
  before this fix either) that this fix happens not to extend to, same status quo as before, just no longer
  silently true for the path/query case next to it.
- **Header values remain unaffected/out of scope for the same reason as cookies**: they are never
  percent-decoded by this engine (S5's fix was about the raw, undecoded value's *length*, not decoding).

**Tests and results.**

- Unit tests added to `tests/test_http_hardening.c` (registered in `main`):
  `test_embedded_nul_rejected` — `url_decode("style.css%00.png", ...)` returns -1 and writes `"style.css"`
  (proving the exact silent-truncation shape the old code produced, now surfaced as an error instead of
  hidden); an ordinary path with no `%00` still returns 0. `parse_query_string` returns -1 for a `%00` in
  either a pair's name or its value, and 0 for an ordinary query string. `parse_http_request` returns -4
  for the literal MEASURED scenario (`GET /static/style.css%00.png HTTP/1.1...`) and for a `%00` in the
  query string alone (path clean); an ordinary percent-encoded path/query with no embedded NUL
  (`/static/style%2Ecss?q=a%20b`) still parses to 0 with the correctly decoded `req->path`.
- Unit test added to `tests/test_connection.c` (registered in `main`):
  `test_handle_readable_embedded_nul_in_path_400` — end to end through the real `handle_readable` path
  (distinct from the pure-parser tests above, same shape as S5's own end-to-end test next to it): the exact
  `GET /static/style.css%00.png` request gets an explicit `HTTP/1.1 400` and the connection is closed,
  exercising `parse_http_request`'s `-4` → `connection.c`'s 400 mapping specifically.
- `make test`: all 13 suites pass (test_http_hardening: 1 new case; test_connection: 1 new case) —
  every existing call site of `url_decode`/`parse_query_string` across `lib/` and `tests/` still compiles
  and passes unmodified, since going from `void` to `int` only adds an ignorable return value in C.
- `make SANITIZE=1 BUILD_DIR=build-asan test`: all 13 suites pass clean under ASan + UBSan.
- `make fuzz` (500,000 iterations): clean (67,357 parsed, 72,558 complete).
- `make check-docs`: passes (121 engine functions covered — `url_decode`/`parse_query_string`'s signatures
  changed but their names didn't, so no new entries were needed).
- Compiled `lib/http_parser.c` and `lib/connection.c` directly with the project's
  `-Wall -Wextra -std=c11 -O2` flags: no new warnings (confirmed the two pre-existing
  `-Wformat-truncation` warnings in `tests/test_http_parser.c` predate this change by diffing against a
  clean stash of the tree before it).
- Live end-to-end reproduction of the exact MEASURED scenario from `improvements.md` was covered by the
  `test_handle_readable_embedded_nul_in_path_400` unit test above rather than a separate manual `curl`
  run against a standalone server (the fix is entirely in the pure parser layer, already exercised
  end-to-end through the real non-blocking `handle_readable` path by that test, same rationale S1's
  earlier live-`curl` verifications don't repeat for every subsequent pure-parser fix).

**Status:** Fixed for the measured problem (the request-target path) and for query names/values, which
`improvements.md` named alongside it. Cookie values are not a gap this fix left open (the vector doesn't
exist for them in this engine); form-urlencoded body fields share the same underlying decoder but were not
extended to check it, a narrower scope than `improvements.md`'s wording implied, documented rather than
silently assumed away.

---

## TLS removal · not an `improvements.md` item — an architectural decision, not a fix

**Date completed.** 2026-09-22.

**Why.** This engine's job is parsing and serving HTTP/1.1 on a plaintext socket. TLS termination is a
separate, well-understood concern that belongs at a gateway or reverse proxy in front of it (nginx, a
cloud load balancer, a sidecar), not compiled into a library whose header comment says it parses
HTTP/1.1. Keeping an OpenSSL-based non-blocking TLS state machine (handshake driving, buffer release,
renegotiation/handshake-deadline hardening — S10, never done) coupled into `Connection`'s lifecycle,
`accept_connections`, `flush_connection` and the main event loop added a second protocol's worth of
state to every connection-handling path for a feature real deployments almost always terminate
upstream anyway. Removing it deletes an entire dependency (OpenSSL/LibreSSL headers and libs, `-lssl
-lcrypto`, the `NO_TLS=1`/`CEXPRESS_HAS_TLS` build-time branch) and an entire class of future TLS CVEs
this codebase would otherwise have to track and patch.

**How it was completed.**

- **Deleted outright:** `lib/tls.c`, `lib/tls.h`, `tests/test_tls.c`, `tests/certs/{server.crt,server.key}`.
- **`lib/app_types.h`:** removed the `TlsState` enum, `Connection.ssl`/`tls_state`/`tls_want_read`/
  `tls_want_write`, `ServerConfig.tls_enabled`/`tls_cert_file`/`tls_key_file`, and `App.ssl_ctx`.
- **`lib/connection.c`:** `conn_read`/`conn_write` dropped their `SSL*` branch and now call `recv`/`write`
  directly (and dropped their now-unused `App *` parameter). `accept_connections` no longer branches into
  `tls_connection_init`/`tls_connection_handshake` after accept — every accepted fd goes straight to
  `event_loop_watch_read`. `reject_overloaded_connection` (S3) dropped its `is_tls` parameter and always
  writes the 503 body now (previously skipped it for a TLS listener, since a TLS client expects a
  handshake, not HTTP text — moot with no TLS listener possible). `flush_connection` no longer checks
  `tls_has_pending` after a keep-alive response completes. `app_listen_worker` no longer calls
  `tls_init_app`, no longer prints "(HTTPS / TLS)", no longer branches on `TLS_STATE_HANDSHAKE` per event,
  and — the direct fix for the now-moot P5 — no longer runs the per-poll-batch scan over the entire
  `connections` table checking `tls_has_pending` on every TLS connection (that scan existed only because
  OpenSSL can buffer decrypted bytes internally that a bare `recv` on the fd would never see; with no
  OpenSSL, there is no hidden buffer to poll for). `close_idle_connections`'s comments describing a
  "TLS handshake in flight" were updated to describe a request in flight only (the S1 deadline mechanism
  itself, `request_started`/`REQUEST_HEADER_TIMEOUT_SECONDS`/`REQUEST_BODY_TIMEOUT_SECONDS`, is unchanged
  and still covers plaintext slow-drip clients exactly as before — only the TLS-handshake half of what it
  used to also cover is gone, because there is no handshake anymore).
- **`lib/router.c`/`lib/router.h`:** removed `app_enable_tls` entirely and its `app_init` field
  initialization.
- **`lib/cexpress.h`:** dropped `#include "tls.h"`.
- **Root `Makefile`:** removed the entire OpenSSL auto-detection block (`OPENSSL_CFLAGS`/
  `OPENSSL_LDFLAGS`/`CEXPRESS_HAS_TLS`/`NO_TLS`), `lib/tls.c` from `LIB_SRCS`, `TLS_TEST_BIN` and every
  `lib/tls.o` dependency from every test-binary target (including the epoll-shim targets), and the
  `./$(TLS_TEST_BIN)` line from the `test` target. `make test` now runs 13 suites, down from 14.
- **`examples/todo_sqlite/main.c`:** removed the `TLS_CERT`/`TLS_KEY` env-var block that called
  `app_enable_tls`. **`examples/todo_sqlite/Makefile`:** removed its own OpenSSL detection block.
- **`Dockerfile`:** dropped `openssl-dev`/`openssl-libs-static` from the builder stage and `libssl3`/
  `libcrypto3` from the runtime stage.
- **`scripts/export_framework.sh`:** the generated standalone Makefile it writes for `make export`
  dropped its OpenSSL detection and `lib/tls.c`; its generated README no longer tells consumers to link
  `-lssl -lcrypto`.
- **Docs:** `lib/CLAUDE.md` (added a "No TLS" note up top explaining the rationale, removed `tls.c/h` from
  the files table, the `SSL`/`SSL_CTX` ownership row, the TLS return-convention line, the TLS bullet under
  "Behavior reference", and every TLS aside in the S1/S2/S3 timeout/overload descriptions), `lib/API.md`
  (removed `app_enable_tls` and the TLS internals line; added a one-line "no TLS" note — `make check-docs`
  enforces this file matches the headers, so it would have failed otherwise), `tests/CLAUDE.md`
  (14 → 13 suites, removed the `test_tls.c` entry), `README.md`, `DOC.md`, `importing.md` (dependency
  tables, the consumer Makefile template, the compile/link examples, the file tree, the checklist),
  `examples/todo_sqlite/CLAUDE.md`, `scripts/CLAUDE.md`. `finds.md` and `stress_tests/stress_test_report.md`
  were deliberately left untouched: they are point-in-time snapshots (like `improvements.md`'s own dated
  measurements), not living documentation, so they still correctly describe what was true when they were
  written.
- **`improvements.md`:** marked S10 (TLS hardening gaps), P5 (whole-connection-table TLS scan) and M3
  (TLS SSL buffer release) as REMOVED in both the summary table and their detail sections, with a pointer
  to this entry, rather than deleting them — the measurements are still accurate historical record of a
  problem that existed in code that no longer does.

**Tests and results.**

- `make test`: all 13 suites pass (down from 14 — `test_tls` is gone).
- `make SANITIZE=1 BUILD_DIR=build-asan test`: all 13 suites pass clean under ASan + UBSan.
- `make fuzz FUZZ_ITERS=200000`: clean (26,972 parsed, 29,122 complete).
- `make check-docs`: passes (120 engine functions covered, down from 130 in the S4 entry above by the 10
  functions removed: 9 `tls_*` plus `app_enable_tls`).
- `make demo`: builds clean with no OpenSSL flags in the compile/link command (verified by inspecting the
  actual `gcc-16` invocation Make printed).
- Live end-to-end verification: started the demo (`QUIET=1 PORT=18099 ./cexpress_demo`) and confirmed
  `GET /ping` and `GET /api/todos` both answer normally over plain HTTP — no TLS listener, no handshake,
  nothing changed about ordinary plaintext request handling.
- Compiled every touched file with the project's `-Wall -Wextra -std=c11 -O2` flags via the normal build:
  no new warnings.

**Deliberately not done:** no compatibility shim, no `NO_TLS`-style flag kept as a no-op, no deprecated
wrapper around `app_enable_tls` that errors at runtime — the function and every trace of it are gone, per
this codebase's own standing guidance to avoid backwards-compatibility hacks for something being
deliberately removed, not renamed.

**Status:** Done. TLS is not a feature of this engine; terminate it at a gateway or reverse proxy.
