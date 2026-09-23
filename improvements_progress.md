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

## S7 · A failing worker is respawned instantly, forever

**Date completed.** 2026-09-22.

**How it was completed.**

`cluster_listen` (`lib/cluster.c`) used to respawn any worker that exited abnormally immediately, with no
delay and no limit — MEASURED (`improvements.md`, S7) 10,594 respawns and 31,788 log lines in about 4
seconds with `WORKERS=2` against a port already taken. Both halves of `improvements.md`'s fix were
implemented, not just one:

- **Preflight bind check (the "Better:" fix).** `cluster_listen` now calls `create_server_socket(port)`
  once itself, before forking anyone, and immediately closes the returned fd - a pure validation call.
  `create_server_socket` already `perror`s and `exit()`s (S12: still unfixed there - a real error-code
  path is a larger, separate change this fix does not attempt) on any bind/listen failure, so this one
  call turns the exact MEASURED scenario (every one of `workers_count` children failing the identical
  `bind()` and getting respawned instantly) into a single clear message and one process exit, before a
  single `fork()` happens. This directly targets the *fatal, permanent misconfiguration* case (wrong
  port, permission denied on a privileged port, ...), which no amount of backoff can ever fix.
- **Exponential backoff + a restart budget (the base fix), for a worker that starts fine and later
  crashes at runtime** — the case the preflight check above does not cover. New per-slot state
  (`ClusterWorkerSlot.pending_respawn` / `backoff_until_ms` / `consecutive_failures` / `window_start_ms`)
  and a new helper, `record_worker_failure`, shared by three call sites (an abnormal exit reaped by the
  supervision loop's `waitpid`, a `fork()` failure while respawning a previously-failed slot, and a
  `fork()` failure in the very first spawn loop — this last one was a pre-existing, separate gap where a
  slot whose initial fork failed was simply abandoned forever with no accounting or retry at all; folding
  it into the same mechanism was a small, essentially free extension once the helper existed, not a
  deliberate independent fix):
  - **Backoff**: 100 ms (`CLUSTER_RESTART_BACKOFF_INITIAL_MS`) doubling per consecutive failure, capped
    at 30 s (`CLUSTER_RESTART_BACKOFF_MAX_MS`) — `improvements.md`'s own suggested numbers.
  - **Budget**: more than `CLUSTER_RESTART_BUDGET` (5) failures within a **sliding**
    `CLUSTER_RESTART_WINDOW_MS` (60 s) window — not a lifetime count, so an occasional, unrelated crash
    over a long-running server's life doesn't eventually trip a budget that was really meant to catch a
    *loop*. The window resets whenever a failure falls outside it (`now - window_start_ms >
    CLUSTER_RESTART_WINDOW_MS`), same "reset on staleness" idea as S1/S4's own deadline fields.
  - **Non-blocking scheduling**: the master's supervision loop still polls every ~50 ms
    (`waitpid(WNOHANG)` + a short `nanosleep` when idle, unchanged); a new pass each iteration checks
    every `pending_respawn` slot against `monotonic_ms() >= backoff_until_ms` and respawns exactly those
    that are due. A single blocking `nanosleep` for the backoff duration was deliberately rejected as a
    design (considered and discarded, not just not implemented): it would serialize concurrent slot
    failures against each other and make the master unresponsive to SIGINT/SIGTERM for up to 30 s.
  - **When the budget is exhausted**, the affected slot's `record_worker_failure` call returns 1; the
    supervision loop treats that exactly like an external SIGTERM to itself (`g_shutdown_signo = SIGTERM`
    if not already set) and falls into the *same* pre-existing drain sequence (forward SIGTERM to
    remaining active workers, wait up to 6 s, SIGKILL if needed) rather than a separate code path, then
    calls `exit(EXIT_FAILURE)` at the very end - `cluster_listen` is `void` with no way to hand a failure
    back to `app_listen`/`main()`, so this is the same "library calls `exit()` for a fatal condition"
    convention `create_server_socket` and `event_loop_init` already use (S12, again not attempted here).
- **Incidental fix, found while writing the tests below**: `spawn_worker` now `fflush(stdout)` /
  `fflush(stderr)` right before `fork()`. `printf` to a pipe (not a tty) is fully buffered, not
  line-buffered; without this, an unflushed line in the master (e.g. the "starting N workers" banner) got
  duplicated once per subsequent child's own `exit()`-time flush, since `fork()` copies the buffer
  contents as-is. Harmless before this fix (each slot only ever forked once, immediately, so there was
  nothing subsequent to duplicate into), but S7's own respawn loop forks the same slot repeatedly in quick
  succession, which made the pre-existing duplication newly visible and newly relevant - exactly the kind
  of log noise a crash-loop fix should not be adding back in a different form.

**Deliberately scoped down**, matching the S1–S6 precedent of narrowing rather than silently doing less
than advertised:
- **S12 (making `create_server_socket`/`event_loop_init` return error codes instead of calling `exit()`)
  is explicitly out of scope** — `improvements.md`'s own S7 entry names it as the more thorough version of
  this fix and lists it separately with its own ID. The preflight check above reuses `create_server_socket`
  precisely because it already does the right thing (print one clear message, stop), not because this fix
  changed how it fails.
- **The restart budget's numbers (5 failures / 60 s window, 100 ms–30 s backoff) are fixed compile-time
  constants** (`cluster.c`-local `#define`s, not `ServerConfig` fields, not `app_types.h` - same
  convention as `connection.c`'s `ARENA_SIZE`), not configurable per application. `improvements.md` only
  asked for "for example" values; no attempt was made to expose them, since nothing in this codebase's
  existing cluster API took a tuning knob for anything like this either.
- **A per-slot budget, not a whole-cluster one**: two different slots each get their own independent
  5-failures-per-60s allowance. A pathological scenario where every slot fails exactly 5 times and no more
  (staying just under each individual budget) while the cluster is effectively never fully healthy is a
  known accepted limitation of a per-slot design, not something this fix attempts to close - matching
  `improvements.md`'s own fix description ("Exponential backoff **per slot**").

**Tests and results.**

- Unit tests added to `tests/test_cluster.c` (registered in `main`), both driving a real forked master
  process end to end rather than poking internal state (the new fields are `cluster.c`-local, not exposed
  through `cluster.h`):
  - `test_cluster_master_exits_fast_when_port_is_taken` — occupies the target port on `INADDR_ANY`
    (matching `create_server_socket`'s own bind address exactly; an address mismatch, e.g. loopback-only
    vs. wildcard, can coexist on some stacks without `SO_REUSEPORT` and would have silently defeated the
    test) without `SO_REUSEPORT`, then forks a real master with `workers = 2` against that port and
    asserts it exits non-zero in under 2 seconds - reproducing the exact MEASURED scenario
    (`improvements.md`) and asserting the fix's actual guarantee (fast, single failure) rather than just
    "eventually fails".
  - `test_cluster_master_exits_after_restart_budget_exceeded` — registers an `app_on_worker_start` hook
    that calls `exit(7)` immediately (a worker that can never come up, deterministically and as fast as
    possible, before any socket work), forks a real master with `workers = 2`, and asserts it eventually
    exits with status 1 - exercising the full backoff-then-give-up path for real (the test genuinely waits
    through the 100/200/400/800/1600 ms schedule, not a mocked clock), confirmed by manual runs showing
    the expected failure-count/backoff-ms log lines in order before the final "giving up" message.
- `make test`: all 13 suites pass (test_cluster: 2 new cases). Needed a real hung-process debugging pass
  during development: the first version of `test_cluster_master_exits_fast_when_port_is_taken` bound its
  blocker socket to `INADDR_LOOPBACK` while `create_server_socket` binds `INADDR_ANY` — on this machine
  the two bindings didn't conflict, so the preflight check correctly found the port free, the master
  proceeded to spawn two real, HTTP-serving workers, and the test hung forever waiting for a fast failure
  that was never going to happen. Fixed by matching `INADDR_ANY` exactly (see Tests above); left as a
  cautionary note here since it is the kind of test bug that produces a false sense of coverage rather
  than an outright failure.
- `make SANITIZE=1 BUILD_DIR=build-asan test`: all 13 suites pass clean under ASan + UBSan (the two
  pre-existing `-Wformat-truncation` warnings in `tests/test_http_parser.c`, confirmed via `git stash` to
  predate this change, are unrelated).
- `make fuzz` (300,000 iterations): clean (40,522 parsed, 43,650 complete) - `cluster.c` is not on the
  fuzzed parser/router/response path, included for completeness after touching the tree.
- `make check-docs`: passes (121 engine functions covered - `cluster_listen`'s signature and name are
  unchanged, only its doc comment; `record_worker_failure` and `monotonic_ms` are `static`, not part of
  the public surface `API.md`/`check-docs` track).
- Compiled `lib/cluster.c` directly with the project's `-Wall -Wextra -std=c11 -O2` flags: no new
  warnings.
- Manual verification of the exact log shape (run directly, outside the test harness, to read the timing
  by eye): `WORKERS=2` against an already-bound port now prints exactly one `bind: Address already in
  use` line and exits, instead of thousands of respawn lines; a worker forced to `exit(7)` on every
  attempt (the same hook the automated test uses) prints `failure 1/5` through `failure 5/5` with the
  correct doubling backoff (100/200/400/800/1600 ms) before the sixth attempt prints "worker 0 failed 6
  times within 60s, giving up - shutting down", drains, and the master process exits with status 1.

**Status:** Fixed for both the fatal-misconfiguration case (preflight check; the exact MEASURED problem)
and the general crash-loop case (backoff + per-slot restart budget). S12 (returning error codes instead of
`exit()`ing from `create_server_socket`/`event_loop_init`, and by extension from this fix's own fatal
`cluster_listen` exit) remains open, per `improvements.md`'s own scoping of S7 versus S12 - not a gap this
work introduced, the other half of a two-part fix `improvements.md` itself splits into two IDs.

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

---

## P2 · Request headers are parsed three times

**Date completed.** 2026-09-22.

**How it was completed.**

Before this fix, `handle_readable`'s per-`recv` hot path ran an independent `phr_parse_request` pass for
each of three separate checks — and, after S4 added its own, a fourth: `reject_if_over_body_limit`'s own
`request_framing` call, `request_is_complete`'s own `request_framing` call, and `parse_http_request`'s
*two* passes (its own top-level `phr_parse_request` call, then a second, entirely redundant
`request_framing` call at its old line 327 to recompute the same `header_len`/`content_length`/`chunked`
it could have kept from the first). `improvements.md`'s own MEASURED numbers (`request_is_complete` 165 ns,
`parse_http_request` 538–546 ns for a browser-shaped GET) predate S4; this fix targets the whole chain as
it stands today, not just the two functions originally named.

- **New shared type, `ParsedHead` (`lib/app_types.h`).** One `phr_parse_request` pass's result — the
  tokenized method/path/headers plus the framing verdict (content-length/chunked) — kept in a struct that
  every downstream check can read instead of re-deriving. Its `headers[MAX_FRAMING_HEADERS]` array is
  sized at `MAX_FRAMING_HEADERS` (100), deliberately **larger** than `MAX_HEADERS` (32, what the engine
  actually stores): keeping the underlying `phr_parse_request` call at the old `request_framing` capacity
  (100) rather than dropping it to 32 was not optional — with a 32-header cap, `phr_parse_request` itself
  fails (returns -1, "header capacity exceeded") for any request with 33+ headers, which this refactor's
  `header_len == 0` check (preserving S8's known gap on purpose, see below) would then read as
  "incomplete" and loop forever waiting for more bytes instead of answering 400 the way the pre-fix code
  already did (pre-fix, `request_framing`/`request_is_complete` used a 100-header capacity of their own,
  so a 33+-header request's framing succeeded there and only failed later, correctly, inside
  `parse_http_request`'s own 32-header-capped call). `app_types.h` now includes
  `vendor/picohttpparser/picohttpparser.h` for `struct phr_header`, matching this codebase's existing
  convention of keeping every struct in a dedicated header rather than a `.c` file (`CLAUDE.md`, "Coding
  Standards").
- **New functions (`lib/http_parser.c/h`), added alongside the existing ones, not replacing them:**
  - `parse_request_head(buf, len, ParsedHead *)` — the one `phr_parse_request` call, done once.
  - `request_head_is_complete(ParsedHead *, buf, len)` — `request_is_complete`'s exact logic (chunked
    scan included) against an already-parsed head instead of re-parsing.
  - `parse_http_request_from_head(raw, raw_len, ParsedHead *, Request *, Arena *)` — the population half
    of `parse_http_request` (method/path/query/header/cookie copying, body extraction), given a head
    instead of re-running `phr_parse_request`/`request_framing` on the same bytes. Same return codes,
    ownership and limits as `parse_http_request` (`-1`/`-2`/`-3`/`-4`, `req.body` always NUL after any
    outcome — see Tests below for the regression this had to keep passing).
  - A private helper, `compute_content_length_and_chunked(headers, num_headers, chunked_out)`, factors out
    the Content-Length/Transfer-Encoding scan that used to be inlined once in `request_framing` and
    duplicated a second time, slightly differently, inside `parse_http_request`'s old second pass — there
    is now exactly one copy of this logic, called by `parse_request_head`.
- **The existing public functions become thin, behavior-identical wrappers**, kept for every caller that
  doesn't need more than one piece (tests, `fuzz_parser.c`, anything outside `connection.c`):
  `request_framing` = `parse_request_head` + unpacking its fields into the old out-parameters;
  `request_is_complete` = `parse_request_head` + `request_head_is_complete`; `parse_http_request` =
  `parse_request_head` + (`header_len == 0` ? reset-and-`-1` : `parse_http_request_from_head`). None of
  these three had their signature or documented behavior changed.
- **`connection.c`'s hot path (`handle_readable`) now calls `parse_request_head` exactly once per `recv`**
  and threads the result through `reject_if_over_body_limit` (which dropped its own `request_framing`
  call entirely, now just reading `head->header_len`/`chunked`/`content_length`/`path`), the completeness
  check (`request_head_is_complete`) and the full parse (`parse_http_request_from_head`) — one
  `phr_parse_request` pass total instead of up to four. The rare "buffer full, headers still incomplete"
  branch after the read loop (a separate, infrequent code path, not the common "request completed within
  this recv" one this fix targets) was deliberately left calling `request_framing` on its own — see
  Deliberately scoped down.
- **`tests/bench_hotpath.c`'s `one_request`** (its own comment says "Same sequence as connection.c:
  handle_readable") was updated to match: it now calls `parse_request_head` +
  `request_head_is_complete` + `parse_http_request_from_head`, the same one-pass sequence, instead of the
  two-call `request_is_complete` + `parse_http_request` it used before — so `make bench`'s numbers
  reflect the real fix rather than a partial one.

**Deliberately scoped down**, matching the S1–S7/P1 precedent of narrowing rather than silently doing less
than advertised:
- **No incremental (`last_len`) resume across separate `recv`s.** `improvements.md`'s suggested fix also
  mentioned passing picohttpparser's `last_len` so a request that arrives in several `recv`s doesn't
  re-tokenize bytes it already saw on a prior, incomplete call. Not done: the measured problem (three to
  four passes over the *same* bytes within one `recv`'s worth of data) is what this fix closes; the
  cross-`recv` case needs `ParsedHead` (or at least the partially-parsed header array and byte position)
  to persist *on the `Connection`* across event-loop turns, which is a materially larger change (a new
  `Connection` field, invalidation rules for when `in_buf` moves under `realloc`/`memmove`) for a case
  `improvements.md` itself only calls a PROJECTED, unmeasured, secondary win, not the MEASURED 165–546 ns
  figures the ID is named for. Also explicitly not attempted: `P9` (pipelining), which
  `improvements.md`'s own dependency note says "needs P2's cached `header_len`" — this fix keeps
  `ParsedHead` as a per-`recv` stack local in `handle_readable`, not a `Connection`-resident cache, so
  there is nothing here yet for a future P9 fix to consume across requests on the same connection.
- **The rare buffer-full branch (`handle_readable`, after the read loop) still runs its own
  `request_framing` call**, not the `head` computed inside the loop. Considered hoisting `head` out of the
  loop to reuse it there too, but rejected: that branch can be reached on a call to `handle_readable` where
  the read loop's body never executed at all this call (a connection already sitting exactly at
  `in_cap - 1` from a previous call's `grow_in_buf` "already large enough, wait for more" case — see
  `grow_in_buf`'s own comment) — a hoisted-but-unset `head` would be read as uninitialized memory in that
  case. Left alone: it is a single, infrequent parse (Slowloris-shaped or a body larger than the current
  buffer), not the up-to-four-passes-per-ordinary-request problem this fix targets.
- **`request_framing`/`request_is_complete`/`parse_http_request` are not deprecated or hidden** — every
  test file, `fuzz_parser.c`, and any external code built against this library keeps working unmodified;
  only `connection.c`'s own hot path was moved onto the new, single-pass functions.

**Tests and results.**

- No new test file: this is an internal refactor of already load-bearing code, not new behavior — the
  correctness bar is "every existing test still passes with the exact same assertions," which is a
  stronger check for a change like this than a handful of new cases would be (a subtly wrong refactor
  that still passes new cases written *for* the refactor proves less than one that has to pass tests
  written *before* it existed, especially test_http_hardening.c's `test_parse_does_not_depend_on_zeroed_request`
  and its assertion that `parse_http_request` resets `req.body` to `NULL` even when the very first
  `phr_parse_request` call fails outright — the exact case `parse_http_request`'s new wrapper had to keep
  handling by resetting `req` itself before returning `-1`, without ever reaching
  `parse_http_request_from_head`).
- `make test`: all 13 suites pass, unmodified assertions, including every case in `test_http_parser.c` and
  `test_http_hardening.c` that calls `request_framing`/`request_is_complete`/`parse_http_request` directly
  (the `>32`-header/431/-3/-4/chunked/duplicate-`Content-Length` regressions from S5/S6 and earlier), and
  `test_connection.c`'s S1–S6 end-to-end cases through the real `handle_readable`.
- `make SANITIZE=1 BUILD_DIR=build-asan test`: all 13 suites pass clean under ASan + UBSan — meaningful
  here specifically because `ParsedHead.headers` is a 100-entry array now shared by reference
  (`const ParsedHead *`) across `reject_if_over_body_limit`/`request_head_is_complete`/
  `parse_http_request_from_head` rather than copied per call, the kind of lifetime change a sanitizer run
  is well-suited to catch.
- `make fuzz FUZZ_ITERS=300000`: clean (40,522 parsed, 43,650 complete) — `fuzz_parser.c` exercises
  `request_framing`, `request_is_complete` and `parse_http_request` directly (their signatures are
  unchanged), so this covers the wrapper path, not the new `connection.c` call sequence; `test_connection.c`
  and the live checks below cover that.
- `make check-docs`: passes (124 engine functions, up from 121 — `parse_request_head`,
  `request_head_is_complete`, `parse_http_request_from_head` added to `lib/API.md`).
- Compiled `lib/http_parser.c` and `lib/connection.c` directly with the project's
  `-Wall -Wextra -std=c11 -O2` flags: no new warnings.
- **`make bench` (Apple M3 Pro, gcc-16 -O2, 22 Sep 2026, several runs for stability):** minimal GET
  198 → ~180 ns, browser-shaped GET (10 headers, cookies, query) 825 → ~514–587 ns (**MEASURED −30 to
  −38%**, matching `improvements.md`'s PROJECTED "≈300 ns of 781 ns, −38%" almost exactly), JSON POST
  313 → ~230–247 ns (**MEASURED −21 to −27%**), 404 208 → ~172–193 ns. Minimal GET and 404 move the least
  in absolute terms because they carry only 1–2 headers, so there is less redundant tokenizing per extra
  pass to remove; the browser-shaped and JSON-body cases (more headers, the cases `improvements.md`
  measured) show the larger, PROJECTED-matching wins. `lib/CLAUDE.md`'s "Hot-path rules" section records
  the exact numbers.
- Live end-to-end verification against `examples/todo_sqlite`'s demo (`QUIET=1`, built with `gcc-16`):
  - `GET /ping`, `GET /api/todos` (with the demo's `Authorization: Bearer my-secret-api-key`): 200, exact
    bodies unchanged.
  - A request with 40 headers (`X-Extra-00` … `X-Extra-39`) against `/ping`: **`400 Bad Request`**, not a
    hang — the exact regression this fix's `MAX_FRAMING_HEADERS`-above-`MAX_HEADERS` sizing exists to
    prevent (see Deliberately scoped down / the `ParsedHead` bullet above): with the header-array capacity
    dropped to 32 instead of kept at 100, this same request would have looped forever waiting for more
    bytes instead of answering 400.
  - `GET /static/style.css%00.png` (S6's exact regression shape): still `400`.
  - Two sequential `GET /ping` requests over one kept-alive connection (raw socket, no reconnect): both
    `200`, confirming `body_limit_checked`/`request_started` reset correctly between requests on the new
    path.
  - A `POST /api/todos` with a properly-framed `Transfer-Encoding: chunked` body (raw socket, hand-built
    chunk framing — `curl`'s own `--data-binary` with a manually-set `Transfer-Encoding` header does not
    actually chunk-encode its payload, which is a `curl` behavior, not an engine one): `201 Created` with
    the decoded JSON body reflected back correctly.

**Status:** Fixed for the measured problem (the hot path's redundant `phr_parse_request` passes,
`handle_readable` → `reject_if_over_body_limit`/completeness/full-parse, collapsed from up to four to one).
The `last_len` incremental-resume half of `improvements.md`'s suggested fix, and P9 (pipelining, which
depends on it), remain open — not gaps this work introduced, the larger, PROJECTED-only part of P2's own
fix list that this entry did not attempt, per the scoping above.

---

## P3 · Headers are copied into fixed arrays inside a 19 KB `Request`

**Date completed.** 2026-09-22.

**How it was completed.**

`improvements.md`'s fix had three parts: keep `struct phr_header`-style views into the input buffer
instead of copying every header, materialize a NUL-terminated value lazily on first `req_get_header`,
and parse cookies and query strings lazily too. The header-view half and the cookie half were done in
full; the query half was deliberately not attempted (see Deliberately scoped down) — query storage is a
small fraction of `Request`'s size and S6 already requires validating every query value for an embedded
NUL at parse time regardless of whether a handler ever reads it, so there is no CPU to save by deferring
it, only bytes in an already-small array.

- **Header storage (`lib/app_types.h`: `Request.headers`).** Replaced `header_names[MAX_HEADERS][64]` +
  `header_values[MAX_HEADERS][MAX_HEADER_VALUE_LEN]` (32 × 1,088 bytes = 34,816 bytes) with
  `struct phr_header headers[MAX_HEADERS]` (32 × 32 bytes = 1,024 bytes) — the exact same `phr_header`
  type `ParsedHead.headers` already held, reused rather than inventing a parallel `HeaderView` type.
  `name`/`value` point directly into the connection's `in_buf` (or, for a test calling `parse_headers`,
  into that call's own buffer) and are not NUL-terminated. `MAX_HEADER_VALUE_LEN` (S5's raised 1024-byte
  per-value cap) is deleted outright: a view has no fixed capacity to overflow, so there is nothing left
  for it to bound.
- **Populating the views (`lib/http_parser.c`: `parse_http_request_from_head`).** The old loop's two
  `copy_bounded` calls per header (one for the name, one for the value, each first checked against its
  destination array's size and rejected with S5's `-3` if either didn't fit) became a single struct
  assignment, `req->headers[req->header_count] = head->headers[i]`, run unconditionally since
  `head->num_headers > MAX_HEADERS` is already rejected with `-1` earlier in the same function (the
  `req->header_count < MAX_HEADERS` guard is kept anyway, for the loop to stay correct on its own if that
  invariant ever changes upstream, not because it can currently be tripped).
- **Retiring S5's `-3` (`lib/http_parser.c`, `lib/http_parser.h`, `lib/connection.c`).** Since a view has
  no per-header size limit, the code path that used to produce `-3` (→ 431) is gone — `parse_http_request`/
  `parse_http_request_from_head` never return it any more. The numeric value is not reused for anything
  else (documented in `http_parser.h` as "retired... kept reserved, not reused, so old caller code
  branching on it is merely dead, never wrong") and `connection.c`'s `handle_readable` dropped the
  `parse_status == -3 ? 431` branch from its status-mapping ternary rather than leave dead code behind.
  This is the "proper fix" `improvements.md`'s own S5 entry predicted ("S5 is solved properly only by
  P3"): a header of any length that fits within the pre-existing whole-header-block cap (`BUF_SIZE`, 8
  KiB, unrelated to this change and unchanged) is now accepted and returned in full, superseding S5's
  interim fix of simply raising the fixed-size cap from 255 to 1024 bytes.
- **Lazy materialization (`lib/http_parser.c`: `req_get_header`).** Rewritten to scan `req->headers` for
  a length-and-case-insensitive name match directly against the view (no NUL-termination needed for
  comparison, since both sides have explicit lengths), and only on a match, `arena_alloc`s
  `value_len + 1` bytes and copies the value into it, NUL-terminated. Not cached: a handler reads a given
  header at most a handful of times per request, so re-copying a few dozen bytes from the arena each call
  costs less than a cache field (and the invalidation rule it would need) would. Requires
  `req->arena != NULL` (see below); returns `NULL` without allocating otherwise, matching "absent" rather
  than crashing on a `Request` no parse function ever populated.
- **`Request.arena` (`lib/app_types.h`), new field.** Set by `reset_request` (called from both
  `parse_http_request_from_head` and `parse_http_request`'s early-malformed path, so it's always set
  after any parse attempt, success or not) to the same `Arena *` `req->body` is allocated from.
  `req_get_header` and `req_get_cookie` (below) use it to materialize NUL-terminated strings out of the
  views/lazy split. A `Request` a test builds and fills entirely by hand (never passed through a parse
  function) has `arena == NULL` from whatever initializer the test used (`memset(0)` in every case this
  entry touched) — `req_get_header` returns `NULL` in that case rather than dereferencing it.
- **Lazy cookie splitting (`lib/http_parser.c`: `req_get_cookie`, `lib/app_types.h`:
  `Request.cookies_parsed`).** `parse_http_request_from_head` no longer calls
  `parse_cookies(req_get_header(req, "Cookie"), req)` itself for every request. Instead,
  `req_get_cookie`'s first call for a given request runs it (`parse_cookies(req_get_header(req, "Cookie"),
  mutable_req)`, then sets `cookies_parsed = 1`); every later call for the same request just scans the
  already-populated `cookie_names`/`cookie_values` arrays. This is a mutable-cache-through-a-const-pointer
  idiom — `req_get_cookie` takes `const Request *req` but casts it back to `Request *` to write
  `cookie_count`/`cookies_parsed`, which is well-defined in C precisely because the underlying `Request`
  object was never actually declared `const` (it's a stack local in `handle_readable`, or a test's own
  variable); only the accessor's *parameter* type is `const`, the same shallow-const situation the
  pre-existing `Request.body`/`arena` pointer fields already relied on. Cookie *storage* itself
  (`cookie_names[MAX_COOKIES][64]`/`cookie_values[MAX_COOKIES][256]`, ~5 KB) is unchanged — only *when*
  the split runs moved, from "every request" to "only a request whose handler actually asks for a
  cookie."
- **`parse_headers` (`lib/http_parser.c`/`.h`), the standalone `void` component parser `tests/` calls
  directly (not on the live request path).** Changed the same way as the live path for consistency and
  because it was little extra work once the view type existed: it now takes an `Arena *` parameter (its
  signature was `void parse_headers(const char *header_block, Request *req)`, now
  `..., Arena *arena)`) and stores views into `header_block` instead of copying into fixed arrays. This
  is a genuine, if incidental, second fix: `parse_headers` used to be the one place in the codebase
  documented as "still truncating" (it had no error path to signal a `-3`-style rejection through, being
  `void`) — with views instead of copies, there is nothing left for it to truncate either, so that
  caveat is gone, not just narrowed.

**Deliberately scoped down**, matching the S1–S7/P1/P2 precedent of narrowing rather than silently doing
less than advertised:
- **Query strings stay eager, fixed-size copies (`req->query_names`/`query_values`, ~2 KB) — not made
  lazy.** `improvements.md`'s P3 fix list also said "parse query strings... on first access," but S6
  requires every query name/value to be percent-decoded and checked for an embedded NUL *at parse time*,
  rejecting the whole request with 400 before a handler ever runs if one is found — deferring the actual
  decode to first access would mean either doing the decode twice (once to validate, once lazily to
  store) or weakening S6's "reject before the handler sees it" guarantee to "reject only if the handler
  happens to read that field," neither of which is what P3 was asking for. Since the array itself is
  small regardless of when it's filled (2 KB, versus headers' pre-P3 34,816 bytes), there was no size win
  available here to chase, only a CPU one that S6 already forecloses. `req->query` (the raw, undecoded
  text after `?`) was already an eager, unconditional copy before this and stays that way — it's a public
  `Request` field applications read directly, not something behind an accessor that could be made lazy.
- **Path parameters (`param_names`/`param_values`, ~1 KB) are untouched.** They aren't filled by the
  parser at all (`match_route` fills them after routing, from the already-decoded `req->path`), so they
  were never part of the "copied eagerly whether or not the handler reads them" problem `improvements.md`
  described — a request either matches a parameterized route, in which case the params are the whole
  reason the route matched, or it doesn't, in which case `param_count` is 0 and nothing was copied.
- **A cookie's own value, once split, can still be truncated to its fixed 255-byte slot.** Making the
  *raw* `Cookie:` header line uncapped (it's a view like every other header now) does not extend to the
  arrays `parse_cookies` splits it into — that's the same pre-existing gap `lib/CLAUDE.md`'s "Known gaps"
  already documented for S5 (a single very long session-token cookie among several shorter ones can be
  truncated even though the header line as a whole fits), carried forward unchanged, not newly introduced
  or newly fixed by this entry.
- **`sizeof(Request)` is 9,792 bytes (`make bench`), not the "~1–2 KB" `improvements.md` speculated.**
  Headers went from 34,816 to 1,024 bytes as intended, but `cookie_names`/`cookie_values` (~5 KB) and
  `query_names`/`query_values`/`param_names`/`param_values` (~3 KB) are unchanged fixed-size arrays, per
  the scoping above — they account for the remaining bulk. A further reduction there would need the same
  view treatment this entry gave headers, deliberately not attempted now.

**Tests and results.**

- Every test file that called the old fixed-array fields or the old `parse_headers(block, req)` two-
  argument signature needed updating (not new behavior on its own, but required for the suite to compile
  against the new `Request` layout — the S1–S7/P1/P2 precedent's own tests all needed the same kind of
  mechanical update when `Request`/`Connection` fields changed shape):
  - `tests/test_http_parser.c`: all 7 `parse_headers` call sites updated to the 3-argument form (`&req,
    &test_arena`); the "no Cookie header" case (`test_parse_http_request`) now forces the lazy cookie
    parse via an explicit `req_get_cookie` call before asserting `cookie_count == 0`, since that count
    reads 0 from `reset_request` regardless of whether anything has actually run the split yet — asserting
    it without first triggering the lazy path would prove nothing about cookie parsing, just about the
    reset default.
  - `tests/test_http_hardening.c`: `parse_headers` call sites updated the same way.
    `test_header_value_whitespace_and_limits`'s old assertion that a 1,100-character header name/value
    was truncated to `req.header_names[0]`/`header_values[0]`'s old fixed sizes was rewritten as
    `test_parse_http_request_header_views_are_not_capped`-style round-trip assertions instead (materialize
    the view's exact 100-char name via `memcpy`, since a view isn't NUL-terminated, then confirm
    `req_get_header` returns the full un-truncated value for it). The old
    `test_parse_http_request_rejects_oversized_header_431` (asserted `-3` for an oversized name/value) was
    renamed `test_parse_http_request_header_views_are_not_capped` and rewritten to assert `0` (success)
    and an exact, uncut round-trip through `req_get_header` for the same oversized name/value inputs —
    the same request shapes, the opposite, now-correct expectation.
  - `tests/test_connection.c`: added `#include "http_parser.h"` (needed for `req_get_header`, not
    previously included since no handler in this file had called an accessor before) and a new
    `auth_header_len_handler` (reflects `req_get_header(req, "Authorization")`'s length into the response
    body, the same pattern as the pre-existing `echo_len_handler`). The old
    `test_handle_readable_oversized_header_value_431` (a 1,100-char `Authorization` value, asserted 431)
    was renamed `test_handle_readable_long_header_value_is_not_capped` and rewritten to register
    `auth_header_len_handler`, assert `200 OK` with the exact expected length in the body
    (`"auth len=1107"`), and assert the connection is *not* closed (keep-alive, not rejected) — exercising
    the fix end to end through the real `handle_readable` path, the same shape S1–S6's own end-to-end
    tests use, not just the pure-parser layer.
  - `tests/bench_hotpath.c`: added `handler_reads_cookie_and_header` (calls `req_get_cookie` +
    `req_get_header`, discards both) and a matching request/route (`/cookie-check`) so `make bench`
    reports both ends of the laziness trade-off — the common case (a handler that never reads a cookie)
    and the "worst case" (one that reads both) — rather than only the faster number, which would have
    been true but incomplete on its own.
- `make test`: all 13 suites pass.
- `make SANITIZE=1 BUILD_DIR=build-asan test`: all 13 suites pass clean under ASan + UBSan — meaningful
  here specifically because `req->headers` now holds pointers into `in_buf`/a caller's buffer that
  outlive the `ParsedHead` they were copied from, the exact kind of dangling-pointer-shaped change a
  sanitizer run is well-suited to catch; it found nothing.
- `make fuzz FUZZ_ITERS=300000`: clean (40,522 parsed, 43,650 complete) — `fuzz_parser.c` already calls
  `req_get_header`/`req_get_query`/`req_get_cookie` after every successful parse, so this exercises the
  view-materialization and lazy-cookie paths against mutated, adversarial input, not just the hand-written
  test cases above.
- `make check-docs`: passes (124 engine functions covered — `parse_headers`'s signature changed but its
  name didn't, so no new/removed entries were needed; `lib/API.md` updated to show the 3-argument form and
  to note headers have no length cap and cookies split lazily).
- Compiled `lib/http_parser.c`, `lib/connection.c`, `lib/app_types.h` (transitively) with the project's
  `-Wall -Wextra -std=c11 -O2` flags: no new warnings.
- **`make bench` (Apple M3 Pro, gcc-16 -O2, 22 Sep 2026, three runs for stability):**
  `sizeof(Request)`: **43,576 → 9,792 bytes (−77.5%)**. Per-request CPU: minimal GET ~160 ns (was 180),
  browser-shaped GET (10 headers, cookies, query; handler never reads either) ~380–420 ns (was 514,
  **MEASURED ≈ −22 to −26%**), JSON POST ~190–210 ns (was 235), 404 ~165–180 ns (was 180). The same
  browser-shaped request against a handler that *does* call `req_get_cookie` + `req_get_header` (the
  "worst case," added to `bench_hotpath.c` specifically to report this honestly rather than only the
  faster number) measured ~460–500 ns — still faster than the pre-P3 514 ns baseline, since header
  storage itself got cheaper independent of whether a handler reads one, though by a smaller margin than
  the common case. `improvements.md`'s own PROJECTED figure for P3 was "a further ~200 ns (browser-shaped)"
  on top of P2's gains; the measured ~100–130 ns reduction here is smaller than that projection but in the
  same direction and of the same order of magnitude — the PROJECTED number was arithmetic on the
  components measured for P3 in isolation ("about 240 ns of the 538 ns... is copying and decoding"), not
  an end-to-end rebuild-and-measure, so some divergence from a real measurement is expected (the same
  caveat `improvements.md`'s own "How to read the gains" table attaches to every PROJECTED figure).
- **Live end-to-end verification against `examples/todo_sqlite`'s demo** (`QUIET=1`, rebuilt against the
  fresh `libcexpress.a`; a stale `SO_REUSEPORT` worker from an earlier, pre-fix build left running on the
  same port during the first pass of this verification produced a misleading 431 for exactly one test
  case before it was noticed and killed — a reminder that `SO_REUSEPORT` will silently load-balance a
  fresh client onto an old, un-rebuilt worker process, not a finding about the fix itself; re-run against
  a clean process tree after `pkill`ing every stale `cexpress_demo`):
  - `GET /ping`: 200. `GET /api/todos` with `Authorization: Bearer my-secret-api-key`: 200 (ordinary
    request-path regression check, unaffected).
  - A 1,100-character `Authorization: Bearer <token>` value (past the old, now-deleted 1024-byte cap):
    **200**, not 431 — the exact MEASURED problem `improvements.md`'s S5 entry described, now provably
    fixed at the storage layer rather than papered over with a bigger cap. A 5,000-character value (an
    order of magnitude past the old cap, still comfortably under the unrelated whole-header-block 8 KiB
    cap): also 200, confirming there is genuinely no per-header cap left, not just a bigger one.
  - `POST /api/todos` with the *correct* API key (short, ordinary length): 201, unaffected. `POST
    /api/todos` with a 1,100-character *wrong* key: 401 — confirming the long value materialized by
    `req_get_header` is the real, exact, correctly-received bytes (not, say, always comparing "long
    enough" as a match, or corrupting the comparison) — `mw_authenticate`'s constant-time comparison
    correctly rejects a wrong key of the same unusual length as a right one.
  - A request with a `Cookie` header the handler never reads (`GET /api/todos` with `Cookie:
    session=abc123; theme=dark` alongside the `Authorization` header): 200, confirming the lazy-cookie
    change doesn't disturb a request that happens to carry cookies without using them.
  - `GET /static/style.css%00.png` (S6's regression shape, unrelated to this fix but re-checked since it
    also flows through `parse_http_request_from_head`): still 400, confirming S6 wasn't disturbed by the
    header/cookie storage change next to it.
  - Two sequential requests over one keep-alive connection (`curl` given two URLs on one invocation):
    both 200, confirming request-to-request state (`cookies_parsed`, `header_count`, the views themselves)
    doesn't leak or dangle across keep-alive reuse of the same connection/arena.

**Status:** Fixed for the two parts of `improvements.md`'s fix list that carried the measured problem —
header views (no copy, no per-header size cap, S5's `-3` retired) and lazy cookie splitting. Lazy query
parsing was deliberately not attempted, per the scoping above (S6's eager-validation requirement forecloses
the CPU win, and the array itself was never the size problem headers were). `sizeof(Request)` landed at
9,792 bytes, well short of `improvements.md`'s "~1–2 KB" speculation but still a 77.5% reduction — the
remaining bulk is cookie/query/param storage, unchanged fixed-size arrays out of this entry's scope, not a
shortfall in the header-view work itself.

---

## M1 · A 64 KiB arena is allocated for every connection

**Date completed.** 2026-09-22.

**How it was completed.**

`connection_create` used to `calloc(sizeof(Connection) + 64 KiB)` per accepted socket and `arena_init` the
trailing 64 KiB as that connection's own arena — MEASURED (`improvements.md`, M1) 24,950 B RSS per idle
keep-alive connection, of which the arena accounted for ~16.5 KB. `improvements.md`'s own fix list named the
real hazard directly: moving to one arena per *worker* only works if two things also change — a still-pending
response's unsent tail has to be copied out of the shared arena before it can safely be reused by another
connection, and file-streaming chunk buffers have to stop living in it entirely. Both were implemented, not
just the headline "one arena, not many" change.

- **`App.arena` (`lib/app_types.h`), new field — the one arena for the whole worker process.** `app_init`
  (`lib/router.c`) `malloc`s its `ARENA_SIZE` (64 KiB, moved here from a `connection.c`-local `#define` since
  `connection_create` no longer needs it at all) buffer and calls `arena_init` once, the same failure
  convention as the pre-existing `app->connections` calloc ("the server can't run without this either" -
  `perror` + `exit(EXIT_FAILURE)`). Allocated before `app_listen`/cluster fork, so every worker process gets
  its own private copy via ordinary `fork()` copy-on-write once it starts writing to it - no different from
  how the rest of `App` already crosses the fork, and unlike a live socket or DB handle (which the existing
  "open DB connections in `app_on_worker_start`, not `main()`" convention is about), a not-yet-written memory
  buffer has no per-process identity to corrupt by being shared before the copy triggers.
- **`Connection.arena` is now `Arena *arena`** (was an embedded `Arena`), set once, at `connection_create`
  (which now takes an `App *` parameter to reach it: `conn->arena = &app->arena`), and never reassigned.
  `connection_create` itself shrank to a plain `calloc(sizeof(Connection))` - no more trailing 64 KiB, no more
  `arena_init` call of its own. Every internal `&conn->arena` became `conn->arena` (already a pointer):
  `parse_http_request_from_head`'s call in `handle_readable`, and `arena_alloc(&conn->arena, ...)` in
  `response.c`'s `send_with_content_type`/`append_to_out_buf`. The public, documented pattern
  `arena_yyjson_alc(&res->conn->arena)` (`lib/API.md`, `lib/examples/cookbook.c`, `examples/todo_sqlite/handlers.c`
  - 12 call sites total) became `arena_yyjson_alc(res->conn->arena)` (drop the `&`) - a mechanical, mostly
  one-character fix at each site, not a design change to any of those functions, and exactly what
  `improvements.md`'s own M1 entry anticipated ("`Connection.arena` can stay as a pointer to the shared arena
  so `res->conn->arena` ... keeps working").
- **The actual hazard: copying out a still-unsent response before the shared arena can be reused
  (`lib/connection.c`: `flush_connection`).** With one arena per connection, a response that couldn't be
  fully written in one `write()` call (a big body, a slow client, a small socket buffer) just sat in that
  connection's own arena until the next writable event drained it - nobody else could disturb it. With one
  arena per *worker*, that stops being true: the very next connection's dispatch would call `arena_reset`
  after its own turn, and a bump allocator's reset doesn't clear memory, it just lets the next allocation
  overwrite it - exactly what a still-pending `out_buf` from a different, unfinished response would sit on
  top of. New field `Connection.out_buf_owned` (0 by default, matching `calloc`'s zero-init - "nothing owned
  yet") tracks this: `flush_connection`'s inner write loop, on `EAGAIN`, now checks whether the current
  `out_buf` is still arena-resident (`!out_buf_owned && out_buf != file_buf` - see below) and if so, `malloc`s
  a buffer exactly the size of the unsent remainder, `memcpy`s it out, repoints `out_buf` at the copy, and
  sets `out_buf_owned = 1` *before* returning control to the event loop. A response that never hits `EAGAIN`
  (the common case - it drains in one shot) never pays for this at all. The owned copy is `free`'d once fully
  drained (a new check right after the outer response loop, before the keep-alive/close branch) or in
  `connection_close`, on whichever exit path gets there first - never both, since the check flips
  `out_buf_owned` back to 0 the moment it fires.
- **File streaming stops using the arena at all (`Connection.file_buf`, new field).** `res_send_file`'s
  chunk-by-chunk body (up to `STREAM_CHUNK_SIZE`, 16 KiB, per `flush_connection` iteration, `read()` from
  `file_fd`) used to `arena_alloc` its chunk buffer fresh each turn - the same hazard as above, just certain
  to happen instead of only possible, since a large file always spans multiple event-loop turns. `file_buf`
  is `malloc`'d once (lazily, on the first chunk) and reused for every subsequent chunk of that one streamed
  response, `free`'d when streaming ends (the pre-existing "`file_remaining == 0`" branch) or in
  `connection_close` if a still-streaming connection closes some other way (a write error, a `read()`
  failure). `out_buf` points at `file_buf` while streaming - never `out_buf_owned` (that flag means "a
  `malloc`'d tail-copy needing the generic free," which `file_buf` is not: it has its own dedicated lifecycle
  and would be a double-free if the generic path also tried to free it - `connection_close` and the
  post-response-loop cleanup both check `out_buf == file_buf` first, before checking `out_buf_owned`, to keep
  the two paths from colliding when both are non-NULL at once).
- **The shared arena is reset by the caller that just used it, not by `flush_connection` itself
  (`handle_readable`, `reject_request`).** `flush_connection` is called from three places: after a fresh
  dispatch (`handle_readable`), after building an error response (`reject_request`, itself called from
  several sites - body-limit 413, parse-failure 400/413/414, buffer-full 431, grow-failure 413/500, and
  `close_idle_connections`' 408s), and from the event loop's own write-readiness dispatch (a `flush_connection`
  call with no dispatch alongside it, just continuing an earlier partial drain). Only the first two just
  finished *using* the shared arena for this request; the third is purely continuing to drain a buffer that,
  thanks to the copy-out/`file_buf` mechanisms above, is already guaranteed not to be arena-resident by the
  time it runs. So `arena_reset(&app->arena)` moved out of `flush_connection`'s old keep-alive branch and
  into `handle_readable` (right after its own `flush_connection` call) and `reject_request` (same), both
  using `app`, never `conn` - `conn` may already be a dangling pointer by then (a non-keep-alive response, or
  a hard write error, both close it inside `flush_connection`), while `app` is always still valid.

**Deliberately scoped down**, matching the S1–P3 precedent of narrowing rather than silently doing less than
advertised:
- **M2 (the 8 KiB `in_buf` per idle connection) was not attempted**, despite `improvements.md`'s own
  dependency note suggesting M1 and M2 "change how `Connection.arena` and `in_buf` are owned (do them
  together)." `in_buf` has a materially different lifetime problem (it needs to survive across several
  `recv()`s for one request, not just across one dispatch-and-flush cycle) that a shared-per-worker treatment
  doesn't solve the same way arena did - genuinely a separate design, left for its own entry.
- **`ARENA_SIZE` (64 KiB) is unchanged.** The per-request working-set assumption behind that number doesn't
  change just because the arena is now shared instead of duplicated - it still only ever holds one request's
  data at a time (reset immediately after), the same as before.
- **No attempt to shrink `App.arena` below 64 KiB or make it configurable.** Out of scope; a different,
  unasked-for change.

**Tests and results.**

- Unit test added to `tests/test_connection.c` (registered in `main`):
  `test_flush_connection_file_stream_survives_another_connections_dispatch` - the direct regression test for
  M1's own stated risk. Streams a file well over `STREAM_CHUNK_SIZE * 4` (`max_flush_bytes`, the
  fairness-yield threshold inside `flush_connection`) to one connection over a `socketpair(2)`, and between
  every yield, runs a completely ordinary, unrelated dispatch-and-flush cycle for a *second* connection on
  the *same* `App` (so it shares `app.arena`) - reproducing the exact "another connection's turn happens
  between this one's turns" sequence the real event loop produces, including that second connection's own
  `arena_reset`. The streamed file's content is a non-repeating hash-of-index pattern (not a short cycle that
  could coincidentally survive corruption undetected), asserted byte-for-byte equal at the end. Also asserts
  `interleaved > 0` (that the fairness-yield genuinely fired at least once), so a future change to
  `STREAM_CHUNK_SIZE`/`max_flush_bytes` that made the file "too small to matter" would fail loudly here
  instead of silently stopping to exercise M1 at all.
- `make test`: all 13 suites pass.
- `make SANITIZE=1 BUILD_DIR=build-asan test`: all 13 suites pass clean under ASan + UBSan - meaningful here
  specifically because this fix replaced "everything lives in one arena until reset" with three different
  manual `malloc`/`free` lifecycles (`out_buf_owned`'s tail-copy, `file_buf`, and the pre-existing arena
  fallback blocks) that all have to interact correctly without a double-free or a leak; a sanitizer run is
  exactly the right tool for that risk profile, and it found nothing across the new test above plus every
  pre-existing streaming/partial-write test in the suite (`test_res_send_file_streams_to_socket`,
  `test_flush_connection_write_stall_reclaimed_by_close_idle_connections`, the S2 8 MiB-buffer test, and
  others).
- `make fuzz FUZZ_ITERS=500000`: clean (67,357 parsed, 72,558 complete) - `fuzz_parser.c` doesn't link
  `connection.c` (no socket layer), but does exercise `response.c`'s changed `arena_alloc(conn->arena, ...)`
  call sites and the `out_buf_owned` resets on every successful parse through `dispatch()`.
- `make check-docs`: passes (124 engine functions covered - no signatures in the public surface changed
  other than `arena_yyjson_alc`'s *usage* at call sites, not its own signature).
- Compiled `lib/connection.c`, `lib/response.c`, `lib/router.c` with the project's `-Wall -Wextra -std=c11
  -O2` flags: no new warnings.
- `make bench`: `sizeof(Connection)` 152 → 144 bytes (the embedded `Arena` struct's several fields replaced
  by one pointer, offset by the two new fields, `out_buf_owned` and `file_buf`); per-request CPU numbers
  unchanged within ordinary run-to-run noise (minimal GET ~150-170 ns, browser-shaped GET ~375-420 ns), as
  expected - the arena mechanics themselves (bump-allocate, reset once per request) didn't change, only
  *which* arena and *when* it's reset.
- **Live reproduction of the exact MEASURED scenario from `improvements.md`** (5,000 idle keep-alive
  connections held open by a script, `ps` RSS delta, `examples/todo_sqlite`'s demo, single worker): baseline
  2,624 KB RSS, 43,760 KB with 5,000 connections held - **8.23 KB per connection**, matching
  `improvements.md`'s own "8,415 B" reference point (its `ARENA_SIZE 0` simulation of exactly this fix) to
  within 2%, and a **−67% reduction** from the ~25 KB/connection this same document measured before the fix.
  Total overhead for 5,000 connections: ~41 MB, matching the PROJECTED "~43 MB" almost exactly. Verified the
  server stayed fully responsive throughout - `GET /ping` and an authenticated `GET /api/todos` both still
  returned 200 while all 5,000 connections were held open.
- **Live correctness check under real concurrent load** (not just the unit test's simulated interleaving):
  wrote a 5 MB file of random bytes into the demo's `public/` directory (over the static-file cache's 256 KiB
  per-entry cap, so `app_serve_static` serves it via the whole-file-in-memory `res_send_bytes` path, an
  arena-resident `out_buf` too large to write in one `write()` call on any real socket), downloaded it over
  `curl` while a concurrent shell loop fired 200 back-to-back `GET /ping` requests at the same worker, and
  confirmed the downloaded file's SHA-256 matched the original exactly - the same shared-arena-survives-
  interleaving property the unit test proves, now confirmed against the real event loop, real sockets, and
  real OS scheduling instead of a hand-driven loop. Temporary file removed after the check; not committed.
- Live cluster sanity check: `WORKERS=2`, five sequential `GET /ping` requests and one authenticated `GET
  /api/todos`, all 200 - confirming `App.arena`'s pre-fork `malloc` (in `app_init`, before `app_listen`'s
  cluster fork) works correctly across `fork()`'s copy-on-write semantics for each worker process
  independently, not just in the single-worker case the rest of this verification used.

**Status:** Fixed for the measured problem (one arena per worker instead of one per connection) and for both
follow-on risks `improvements.md`'s own fix list named (partial-write copy-out, file-streaming buffers) -
neither was left as a known gap. M2 (`in_buf`) remains open, per the scoping above - a related but distinct
change, not a shortfall in this entry's own scope.

---

## P7 · Router child lookup is a linear scan

**Date completed.** 2026-09-22.

**How it was completed.**

`improvements.md`'s own suggested fix ("keep children sorted by segment and binary-search") was implemented
as written, not the hashing/first-byte-index alternative it also floated.

- **`lib/router.c`: two new static helpers.**
  - `compare_seg(a, a_len, b, b_len)` — a total order over segment bytes (short-lexicographic: shared-prefix
    bytes compare first with `memcmp`; if one segment is a strict prefix of the other, the shorter one sorts
    first). This exact comparator has to be the single source of truth for both insert and lookup, or a
    sorted-order mismatch between the two would make binary search silently miss real matches - both now call
    it through one shared function rather than each hand-rolling an equivalent one.
  - `find_child(children, child_count, seg, seg_len, *out_idx)` — binary search over a `children` array kept
    sorted by `compare_seg`. Returns 1 and the matching index on a hit, or 0 and the sorted insertion point on
    a miss (`out_idx` is meaningful in both cases, which is what lets `tree_insert` reuse the same call for
    "does this child already exist" and "where do I put a new one").
- **`tree_insert`'s static-child branch** (`router.c`, was the `for` loop with a `memcmp` at `:508-514`
  plus an always-append at `:515-522`) now calls `find_child` once: a hit reuses the existing child exactly as
  before; a miss grows `children` (unchanged doubling `realloc`) and inserts the new node at the sorted
  position with one `memmove` of the tail, instead of always appending at `child_count`. `param_child` and
  `catch_all_child` are untouched - they were already single pointers, not scanned arrays, so P7 didn't apply
  to them.
- **`tree_search_recursive`'s static-child loop** (was `for (int i = 0; i < node->child_count; i++)` with a
  `memcmp` at `:556-565`) replaced by one `find_child` call. A static segment can only ever have one matching
  child by construction (insert de-duplicates via the same `find_child`), so the loop's "keep scanning after a
  miss" behavior had no case it was actually needed for; a single lookup is exactly equivalent.
- No change to `PatriciaNode`'s layout (`app_types.h`) - `children`/`child_count`/`child_cap` keep their
  existing types and ownership, only the invariant "sorted by `compare_seg`" was added, upheld solely by
  `tree_insert` (the only place that ever writes into `children`).

**Tests and results.**

- New regression test, `test_match_route_many_siblings` (`tests/test_router.c`, registered in `main`): 200
  static sibling routes (`/api/res0` … `/api/res199`) registered in a deterministically shuffled order (not
  sorted, and not reverse-sorted either, to catch an insert bug that only breaks on already-sorted input),
  then looks up the first, second, middle, second-to-last and last registered path plus one path that was
  never registered and is also a *prefix* of real ones textually adjacent to it in sort order
  (`/api/resNotThere` sorts near `/api/res1`/`/api/res19*` under `compare_seg`) - the shape most likely to
  expose an off-by-one in `find_child`'s insertion-point math or a wrong tie-break direction in `compare_seg`.
  All five registered lookups return the exact right route; the unregistered one returns `NULL`.
- `make test`: all 13 suites pass, including every pre-existing `test_router.c` case unmodified (duplicate
  pattern registration, literal-vs-`:param`-vs-`*` precedence, `HEAD`→`GET` fallback, `app_mount` prefixing,
  body-limit prefix matching) - none of that logic sits downstream of child ordering, so this is confirming no
  regression, not testing P7 itself.
- `make SANITIZE=1 BUILD_DIR=build-asan test`: all 13 suites pass clean under ASan + UBSan - meaningful here
  specifically because insertion now does a `memmove` of live `PatriciaNode *` pointers within a `realloc`'d
  array (a spot where an off-by-one would read or write one slot outside the array, exactly what ASan catches)
  instead of the old code's simple append.
- `make fuzz FUZZ_ITERS=1000000`: clean (134,879 parsed, 145,303 complete) - `fuzz_parser.c` exercises
  `match_route` against mutated request paths on every completed parse, so this covers `find_child`'s lookup
  side under adversarial segment bytes (including segments that are prefixes of each other, empty, or contain
  bytes that sort unusually under plain `memcmp`), though the seed routes it registers are few, not thousands.
- `make check-docs`: passes (124 engine functions, unchanged - `compare_seg`/`find_child` are file-static, not
  part of the public API).
- Compiled `lib/router.c` with the project's `-Wall -Wextra -std=c11 -O2` flags: no new warnings.
- **Benchmark reproducing `improvements.md`'s own P7 methodology** (a scratch program outside the repo, C
  program calling `match_route` in a loop after registering N static siblings under one path, matching the
  document's stated method): looking up the last-registered literal route among N siblings, 1,000,000
  iterations, Apple M3 Pro, gcc-16 -O2:

  | Sibling routes | Before (linear scan, `improvements.md`) | After (binary search, this fix) |
  |---|---|---|
  | 10 | 61 ns | 39.0 ns |
  | 100 | 266 ns | 41.3 ns |
  | 1,000 | 2,369 ns | 49.6 ns |
  | 5,000 | 8,401 ns | 80.6 ns |

  **MEASURED** ≈ 104x at 5,000 siblings (8,401 → 80.6 ns) and ≈ 6.4x at 100 (266 → 41.3 ns), both beating
  `improvements.md`'s own PROJECTED "≈80x at 5,000; ≈2-3x at 100" - the after-column includes `match_route`'s
  own per-call overhead (a `strcmp` against the method-tree name, `param_count` reset) on top of the pure tree
  walk, which is why even the 10-sibling case doesn't collapse to a few nanoseconds; that fixed overhead is
  also why the after-column grows sublinearly but not perfectly flat with N (39 → 80.6 ns, not ~39 ns flat) -
  `find_child` itself is O(log N) as designed, but it's a small and shrinking fraction of a lookup that also
  does two path-segment scans and a method-tree string compare.

**Status:** Fixed as scoped. `improvements.md`'s alternative fixes (hashing small segments, indexing by first
byte) were not attempted - sorted-array binary search was the fix it actually recommended and measures within
the range it PROJECTED, so there was no reason to reach for the alternatives. Parameterized (`:name`/`*`)
routes were never part of the problem (single pointers, not scanned) and remain unaffected, as `improvements.md`
itself noted going in.

---

## C4 · macOS `SO_REUSEPORT` does not balance workers

**Date completed.** 2026-09-22.

**Scope, confirmed with the user before implementation.** macOS/BSD only, behind a new compile-time
gate, `CEXPRESS_SINGLE_ACCEPTOR` (`#if !defined(__linux__)`, `app_types.h`). Linux's `SO_REUSEPORT`
4-tuple hashing isn't reported broken (`lib/CLAUDE.md` already called it "expected to work as
designed"), so it keeps its existing per-worker-listens-and-accepts path completely unchanged - zero
new fields touched, zero new syscalls, zero behavior change on that platform. A uniform-everywhere
version was considered and rejected: it would add an extra `sendmsg`/`recvmsg` hop per accepted
connection on the one platform (Linux) that isn't measured to need it, for a problem `improvements.md`
itself scoped as macOS-specific.

**How it was completed.**

`improvements.md`'s own suggested fix - "have the master `accept` and pass the descriptor" - implemented
as a single acceptor: only the cluster master binds and `accept()`s the one listen socket; each
accepted client fd is handed to a worker over a private `AF_UNIX SOCK_STREAM` socketpair via
`SCM_RIGHTS`, round-robin across the currently-active workers.

- **The seam this reused, at no cost to any `event_loop_*.c` backend:** `event_loop_kqueue.c`/
  `event_loop_epoll.c`/`event_loop_io_uring.c` all key `LOOP_EVENT_ACCEPT` purely off
  `fd == app->server_fd` - none of them call `accept()` themselves or know it's specifically a TCP
  listen socket. So a worker's `server_fd` could simply be repointed at its *control socket* (the
  worker-side end of its socketpair with the master) and the exact same readiness event fires when the
  master writes a passed fd to it. Zero lines changed in any `event_loop_*.c` file.
- **`app_types.h`:** the `CEXPRESS_SINGLE_ACCEPTOR` macro (see Scope above), plus `App.accept_via_fd_passing`
  (0 = `server_fd` is a real listen socket, the existing behavior and the only mode on Linux; 1 = it's
  a control socket, new connections arrive as passed fds, never `accept()`ed by this process) and
  `ClusterWorkerSlot.control_fd` (`cluster.c`; the master-side end of a slot's socketpair, `-1` when no
  worker currently occupies it).
- **`connection.c`:**
  - Factored `accept_connections`'s S3/table-growth/`Connection`-setup tail into a new shared static
    helper, `admit_connection(App *, int client_fd)`, deliberately *not* including
    `set_nonblocking`/`TCP_NODELAY` - those are properties of the underlying open file description,
    already set once by whichever side actually calls `accept()` (POSIX: shared across an
    `SCM_RIGHTS` handoff the same way they're shared across `dup()`/`fork()`), so re-applying them
    worker-side would be redundant, not wrong.
  - New `accept_passed_connections(App *)` (`CEXPRESS_SINGLE_ACCEPTOR` only): the `recvmsg`/`SCM_RIGHTS`
    counterpart of `accept_connections`, same drain-until-nothing-pending loop shape, feeding
    `admit_connection` the fd it extracts from each message's ancillary data.
  - `app_listen_worker`'s `LOOP_EVENT_ACCEPT` branch now calls `accept_passed_connections` instead of
    `accept_connections` when `app->accept_via_fd_passing` is set; its one `create_server_socket(port)`
    call is skipped in that case (the fd is already set by the caller below). Every other line -
    `run_worker_init_hooks`, `event_loop_init`, shutdown, the timers - is untouched, so this remains a
    no-op change for every existing caller (standalone mode, Linux workers, every test).
  - New `app_listen_worker_via_control_socket(App *, int control_fd)`: sets `server_fd`/
    `accept_via_fd_passing` and calls `app_listen_worker` - the entry point a `CEXPRESS_SINGLE_ACCEPTOR`
    worker runs instead of `app_listen_worker(app, port)`.
- **`cluster.c`:**
  - `spawn_worker` (renamed the shared fork/child-teardown skeleton to `spawn_worker_common`, called by
    two thin variants - the existing per-worker-bind one on Linux, unchanged, and a new
    `CEXPRESS_SINGLE_ACCEPTOR` one) now creates a fresh `socketpair(AF_UNIX, SOCK_STREAM, 0, sv)` before
    every `fork()` - both the initial spawn and every S7 respawn, since a dead worker's own `sv[1]` died
    with its process. The child branch closes the master's real `listen_fd` (never needed by a worker)
    and every *other* slot's inherited master-side `control_fd` before calling
    `app_listen_worker_via_control_socket` - leaving a sibling's control fd open in this process would
    let it read or write another worker's fd-handoff channel, effectively letting one worker forge
    `SCM_RIGHTS` messages into another's inbound queue.
  - `cluster_listen`'s existing preflight `create_server_socket(port)` call (S7's bind-validation-before-
    forking-anyone check) is kept open instead of closed under this macro - it becomes `listen_fd`, the
    master's one real socket for the cluster's whole lifetime, closed only at final cleanup after every
    worker has drained.
  - New round-robin dispatcher, `dispatch_client_fd`: builds one `sendmsg` with a one-byte data payload
    (a bare `SCM_RIGHTS`-only message is ill-defined on some `AF_UNIX` implementations) plus the fd as
    ancillary data, tries up to `workers_count` active slots starting from a persistent cursor, and
    advances the cursor past whichever slot actually succeeded.
  - The master's supervision loop's unconditional 50 ms `nanosleep` (reached whenever `waitpid` found
    nothing that tick) is replaced with `poll(listen_fd, POLLIN, 50)`: `poll()` returns immediately once
    a connection is pending, so this adds no latency over the old per-worker `accept()` path, while still
    guaranteeing the same ~50 ms upper bound between backoff/`waitpid` scans the plain sleep gave it
    before. On `POLLIN`, drains `accept()` in a loop and dispatches each fd; a fd that can't be dispatched
    (every worker slot down - e.g. mid crash-loop) is just closed, deliberately not duplicating
    `reject_overloaded_connection`'s hand-built 503 in the master to keep this change scoped to C4.
  - `signal(SIGPIPE, SIG_IGN)` added to the master itself: it now writes to worker control sockets
    (`sendmsg`), and a worker that has already closed its end (draining, or dead but not yet reaped) must
    fail that call with `EPIPE`, not take the master down via the default `SIGPIPE` disposition - workers
    already ignored it themselves, the master never had to before this change.
  - Both `socketpair` ends are set non-blocking immediately after creation, before `fork()` - **this was
    the one real bug caught during implementation, not anticipated in the plan**: the event loop's
    readiness contract everywhere else is "drain until `EAGAIN`", and a `socketpair()` fd is blocking by
    default. `accept_passed_connections`'s drain loop calling a blocking `recvmsg` after genuinely
    draining everything pending would block forever instead of returning `EAGAIN`, freezing that
    worker's entire single-threaded event loop - reproduced directly: `make test` hung indefinitely on
    `test_cluster_http_serving_and_shutdown` (the test's own `read()` blocked forever waiting for a
    response nothing would ever send) until this fix, after which it passed immediately. Setting it
    before `fork()` applies to both processes' views of the same underlying open file description
    (`O_NONBLOCK` is a file-status flag, shared the same way across `fork()` as across `SCM_RIGHTS`).

**Deliberately scoped down**, matching the S1-S7/P1-P3/M1 precedent of narrowing rather than silently
doing less than advertised:
- **macOS/BSD only** (see Scope above) - confirmed with the user before writing any code, not a
  unilateral call.
- **No 503 for an unroutable fd in the master.** If every worker slot is down when a connection arrives
  (a narrow, transient window - mid crash-loop before the first respawn lands), the master just closes
  the fd with no response, rather than duplicating `connection.c`'s hand-built 503 responder in
  `cluster.c` as well. The per-worker S3 `max_connections`/`spare_fd` overload logic itself needed no
  changes at all - it already runs after a fd is admitted (now via `accept_passed_connections` instead
  of `accept_connections`), and doesn't care how the fd arrived.
- **`test_so_reuseport_multi_bind`** (the existing raw-socket test of the OS primitive itself) was left
  in place with a clarifying comment, not removed or rewritten - it still validates a true fact about
  the platform, just one the cluster module no longer depends on for balance on macOS.

**Tests and results.**

- New regression, `test_cluster_balances_across_workers` (`tests/test_cluster.c`, registered in
  `main`): a real 4-worker cluster, a handler that echoes `cluster_worker_id()` into the response body,
  40 requests each over its own fresh connection (a fresh ephemeral source port per call, so this
  exercises distribution the same way independent real clients would - relevant even on a
  hypothetically-Linux run of this same test, where the fix doesn't apply but a working `SO_REUSEPORT`
  hash should still show up as more than one worker id). Asserts more than one distinct worker id was
  served - the direct regression for C4 itself: MEASURED (`improvements.md`) that the pre-fix path put
  over 90% of load on a single worker of four, so a naive version of this test would very plausibly have
  seen `distinct == 1`.
- `make test`: all 13 suites pass, including the pre-existing `test_cluster_http_serving_and_shutdown`
  (HTTP end to end through the new single-acceptor path), `test_cluster_master_exits_fast_when_port_is_taken`
  (S7's preflight-bind-failure-exits-fast behavior, unchanged since it's the same `create_server_socket`
  call, just no longer closed afterward), and `test_cluster_master_exits_after_restart_budget_exceeded`
  (exercises the new per-respawn socketpair create/close path six times in quick succession, the
  worker-crashes-immediately-before-ever-touching-a-socket shape).
- `make SANITIZE=1 BUILD_DIR=build-asan test`: all 13 suites pass clean under ASan + UBSan - meaningful
  here specifically because this introduces new fd lifecycle code (`socketpair`, `sendmsg`/`recvmsg`
  with `SCM_RIGHTS`, per-respawn close-then-recreate) that is exactly the class of bug (double-close,
  use-after-close, fd leak) a sanitizer run is well-suited to catch; the restart-budget test's six rapid
  respawns are the most direct exercise of that path and came back clean.
- `make check-docs`: passes (126 engine functions, up from 124 - `accept_passed_connections` and
  `app_listen_worker_via_control_socket` added to `lib/API.md`, both noted there as cluster-internal,
  not meant for application code).
- Compiled `lib/cluster.c` and `lib/connection.c` with the project's `-Wall -Wextra -std=c11 -O2` flags:
  no new warnings.
- **Live verification, instrumented (temporary debug counters, added and removed for this
  verification only - not part of the shipped diff):** `examples/todo_sqlite` demo, `WORKERS=4`,
  `wrk -t8 -c5000 -d6s` against `/ping`. Every dispatched fd was counted per worker in `cluster.c`
  itself: **824 / 824 / 824 / 824 across all four workers, 0 dispatch failures** - a dead-even split,
  not merely "improved."
- **Live verification, `scripts/stress_test.sh`** (the same script and methodology the user ran against
  the pre-fix build earlier in this conversation): `WORKERS=4`, `GET /ping` keep-alive, `wrk -c5000`.
  "Largest single process" RSS as a fraction of "total across 5 processes" (master + 4 workers) went
  from **~88-90%** (pre-fix, both `improvements.md`'s own MEASURED figure - 124.3 of 133.7 MB - and the
  user's own run earlier this conversation - 42.1 of 47.8 MB) to **~24-25%** post-fix (12.2-12.3 of
  50.2-50.3 MB) - within noise of the ideal 20-25% a truly even four-way split (plus the master's own
  small footprint) would produce.
  **One caveat surfaced by this same live testing, not a regression this fix introduced:** the demo's
  own build wiring (`make demo`, `examples/todo_sqlite/Makefile`) did not always relink
  `cexpress_demo` after `lib/cluster.c`/`lib/connection.c` changed and `build/lib/libcexpress.a` was
  re-archived - confirmed by `strings`-checking the binary for debug output that should have been
  present and finding it missing until the stale binary was removed and rebuilt from scratch. Purely a
  verification-workflow snag (a stale binary silently serving old code), not a change to any shipped
  file; not investigated further here since it's outside C4's scope, but worth knowing if a future fix's
  live verification via `make demo` looks unexpectedly unchanged.
- The full `scripts/stress_test.sh` battery (all four phases: ping, churn, read, write) was not run
  end to end for this verification - a run past the `churn`/`conn: close` phases hit a `set -e` abort
  partway into the `read` phase on an unrelated, pre-existing script path (not reproduced or diagnosed
  further; the script's own comments already document `churn`-phase `TIME_WAIT` flakiness on macOS).
  The `ping`-phase numbers above are the direct, apples-to-apples comparison against the user's own
  pre-fix run and are sufficient to confirm the fix; the demo's own end-to-end correctness (HTTP
  responses, not just balance) is separately covered by `test_cluster_http_serving_and_shutdown`.

**Status:** Fixed for the measured problem on its stated platform (macOS/BSD). Linux is untouched by
design, confirmed with the user before implementation - not a gap, the deliberate scope. No 503 path
for the master's own "no worker available" edge case, per the scoping above.

---

## S8 · A malformed request line gets no response

**Date completed.** 2026-09-23.

**How it was completed.**

By the time this was picked up, `improvements.md`'s own problem description was already partly stale:
P2 had since split the old `request_is_complete`/`parse_http_request` duplication into `parse_request_head`
(one `phr_parse_request` pass, filling a `ParsedHead`) plus `request_head_is_complete` (the completeness
check against that already-parsed head). The bug itself hadn't moved, just its address - `request_head_is_complete`
(`lib/http_parser.c`) checked `header_len == 0` before `content_length < 0`, so a request line
picohttpparser rejects outright (`GET /\r\n\r\n` with no HTTP version, `HTTP/2.0`, plain garbage) - which
leaves `header_len` at 0, indistinguishable at that field alone from a request that's merely still
arriving - was reported "need more bytes" forever instead of "done, and it's bad." The connection just sat
there: no reply until the 8 KiB buffer filled (431) or a timeout fired (408, or nothing at all until S1/S2
existed).

- **`lib/http_parser.c`: `request_head_is_complete`.** Swapped the two checks: `content_length < 0` is now
  tested before `header_len == 0`. This is safe specifically because `parse_request_head` (the function that
  fills `ParsedHead`) already arranges for the two `header_len == 0` cases to be distinguishable through
  `content_length`: on `phr_parse_request`'s `-2` (genuinely incomplete), `content_length` is left at its `0`
  default and the function returns immediately; on `-1` (malformed), `content_length` is explicitly set to
  `-1` before returning. So checking `content_length < 0` first catches exactly the malformed case and lets
  a truly incomplete request fall through to the unchanged `header_len == 0` → "need more bytes" branch
  right after it - the fix does not widen "malformed" to swallow "incomplete anywhere near this", it only
  reorders two comparisons that were already computing the right values.
- **`lib/http_parser.c`: `parse_http_request_from_head`.** Once `request_head_is_complete` starts reporting
  "complete" for a malformed request line, `connection.c`'s `handle_readable` calls this function with a
  `head` whose `header_len` is `0` and whose `method`/`path` are `NULL` (set that way at the top of
  `parse_request_head`, never overwritten on the `-1` path) - previously unreachable, since the only caller
  that fed this function a possibly-incomplete-or-malformed head guarded on completeness first, and
  completeness used to imply `header_len > 0`. Added a check at the very top, right after `reset_request`:
  `if (head->header_len == 0) return -1;`, before either `head->method` or `head->path` is touched by
  `copy_bounded`/`memchr` - the standalone wrapper `parse_http_request` already had an equivalent guard of
  its own (pre-existing, for a different reason: defending itself against being called directly on an
  incomplete buffer), but `parse_http_request_from_head` is what `connection.c`'s real per-request path
  calls directly, bypassing that wrapper entirely, so it needed the same guard added explicitly.
- **Comments only, no behavior change:** three stale comments describing this as an open, deliberately-not-widened
  gap (`parse_request_head`'s `res == -1` branch, `parse_http_request_from_head`'s `MAX_HEADERS` check, and
  `lib/http_parser.h`'s doc comments for `parse_http_request_from_head` and `request_is_complete`) were
  updated to describe the fixed contract instead of the gap. `lib/CLAUDE.md`'s "Known gaps" bullet for this
  was removed and its "Return conventions" / "Behavior reference, Request parsing" sections updated to match.

**Deliberately not done:** `improvements.md`'s own fix sketch ("check `content_length < 0` before
`header_len == 0`, two lines") undersold the actual change by one function - the `request_head_is_complete`
reorder alone would have made `handle_readable` call `parse_http_request_from_head` with a `NULL`
`head->method`/`head->path`, and `copy_bounded(dst, dst_size, NULL, 0)` (`len < dst_size` since both are 0)
is a `memcpy` with a `NULL` source pointer at length 0 - technically undefined behavior per the C standard
regardless of the zero length (some UBSan builds flag a `nonnull`-attributed libc function called with a
null pointer even at size 0), not merely a hypothetical: this project already runs `make SANITIZE=1
BUILD_DIR=build-asan test` as a matter of course, so shipping that risk untested wasn't acceptable. The
second guard closes it structurally instead of relying on `memcpy`'s size-0 case being harmless in practice
on this toolchain.

**Tests and results.**

- Unit test added to `tests/test_http_hardening.c` (registered in `main`):
  `test_malformed_request_line_rejected` - three shapes (no HTTP version, `HTTP/2.0`, plain garbage) each
  asserted through `request_framing` (`-1`, `header_len == 0`, unchanged), `request_is_complete` (now `1`,
  the actual regression - was `0` before this fix), and `parse_http_request` (`-1`, unchanged, since its own
  pre-existing guard already covered the wrapper). Also asserts a genuinely incomplete request line (`GET /
  HTTP/1.1\r\n`, no terminator yet) still reports `request_is_complete() == 0` and
  `request_framing() == 0` - the fix must not widen "malformed" to cover this, and this is the regression
  test for that.
- Unit test added to `tests/test_connection.c` (registered in `main`):
  `test_handle_readable_malformed_request_line_400` - the actual, previously-unreachable code path:
  `GET /\r\n\r\n` through the real `handle_readable` gets an explicit `400 Bad Request` and the connection is
  closed immediately, rather than the pre-fix behavior of no reply and an open socket
  (`improvements.md`'s own MEASURED reproduction of exactly this).
- `make test`: all 13 suites pass (test_http_hardening: 1 new case; test_connection: 1 new case).
- `make SANITIZE=1 BUILD_DIR=build-asan test`: all 13 suites pass clean under ASan + UBSan - the specific
  concern the second guard (above) exists to rule out.
- `make fuzz` (1,000,000 iterations): clean. Note the "complete" count jumped from roughly 145,303/1,000,000
  (a prior run recorded elsewhere in this document, different code state) to 783,750/1,000,000 here - this is
  the fix working as intended, not a regression: `request_is_complete` now correctly reports "complete" for
  every malformed mutation the fuzzer generates, not just well-framed ones, so a much larger share of its
  random byte-flips (which land on "unparseable garbage" far more often than "a valid request with a
  slightly-off Content-Length") now short-circuit to `parsed_ok`'s `parse_http_request` call instead of being
  silently skipped as "incomplete" forever. The fuzzer's own oracle is unchanged (memory safety only; it does
  not assert that every input is answered - `improvements.md`'s T1 - so it does not comment on this jump on
  its own, but nothing about the run indicates a memory-safety finding).
- `make check-docs`: passes (126 engine functions - no public API changed, only internal function bodies and
  doc comments).
- Compiled `lib/http_parser.c` directly with the project's `-Wall -Wextra -std=c11 -O2` flags: no new
  warnings.
- Live verification via a standalone probe program linking `lib/http_parser.c` directly (not part of the
  shipped diff): confirmed `request_framing`/`request_is_complete` return `(-1, header_len=0, complete=1)`
  for all three malformed shapes and `(0, header_len=0, complete=0)` for a genuinely incomplete request line,
  matching the unit tests above.

**Status:** Fixed. No hot-path cost (the change reorders two existing comparisons and adds one branch that
only ever executes on a request already destined for rejection); `make bench` was not re-run since neither
changed function is on the path any well-formed request takes.

---

## S11 · Parser accepts ambiguous framing that proxies may read differently

**Date completed.** 2026-09-23.

**How it was completed.**

Two independent ambiguities, both in `lib/http_parser.c`, matching `improvements.md`'s own split of the
problem into "bare `\n`" and "substring `chunked`".

- **Bare `\n` line endings (`has_bare_lf`, new static helper).** picohttpparser tolerates a lone `\n` as a
  line terminator anywhere one is expected - the request line, any header line, and the final blank line
  (verified by reading `vendor/picohttpparser/picohttpparser.c`'s line-ending checks: every one of them is
  `if (*buf == '\r') { ...; EXPECT '\n'; } else if (*buf == '\n') { ... }`, accepting the bare form in all
  three places, not just between headers). A front proxy reading strictly per RFC 9112 (CRLF only) would
  not extend the same tolerance, so the two could disagree about where one request ends and the next
  begins. `parse_request_head` now calls `has_bare_lf(buf, header_len)` right after `phr_parse_request`
  successfully locates the header block (i.e. only once a complete block is known - scanning a
  still-incomplete prefix would flag a lone `\n` whose pairing `\r` just hasn't arrived yet) and, if any
  `\n` in that block is not immediately preceded by `\r`, treats the whole request as malformed: same
  contract S8 established (`header_len` reset to 0, `content_length` set to `-1`, function returns `-1`),
  which `request_head_is_complete`/`parse_http_request_from_head` already handle correctly (report
  "complete" immediately, reject with 400) - no changes needed to either for this half of the fix, they
  already do the right thing for any malformed-request-line shape since S8.
- **Transfer-Encoding matched by substring (`is_sole_token`, new static helper, replacing the loop in
  `compute_content_length_and_chunked` and `scan_framing`).** The old check was
  `strncasecmp(q + k, "chunked", 7) == 0` slid across the whole header value byte by byte - true for
  `Transfer-Encoding: xchunked` (matches at offset 1) and for `Transfer-Encoding: chunked, gzip` (matches
  at offset 0, then the trailing `, gzip` was never looked at again). `is_sole_token` instead tokenizes on
  commas, trims OWS per token, and returns true only when there is **exactly one** non-empty token and it
  case-insensitively equals the target - reused by both the live per-request path
  (`compute_content_length_and_chunked`, which now flags a new `te_unsupported` local instead of setting
  `*chunked_out` on any match) and `scan_framing` (the separate, second implementation backing the public
  `extract_content_length`/`request_has_chunked_encoding` accessors - P2's own comment already notes these
  are intentionally a second copy, not reachable from the live request path).
  **Design decision (not explicitly spelled out in `improvements.md`'s two-line fix sketch): the "final
  token" wording was resolved as "the *only* token".** RFC 9112 §6.1 technically permits something like
  `Transfer-Encoding: gzip, chunked` (chunked must be the last-applied/outermost coding, but an inner
  coding like gzip is syntactically legal) - a fully spec-compliant implementation could accept that and
  hand the still-gzipped, now-dechunked bytes to the application. This engine does not decode `gzip` or
  any other content coding anywhere (`lib/CLAUDE.md`'s "Known gaps": "No HTTP/2, `Expect: 100-continue`,
  compression, ..."), so accepting a coding it will never actually apply and silently dechunking anyway
  seemed more likely to mislead an application (which would receive gzip-compressed bytes with no signal
  that they need further decoding) than to help a real client - "deny by default" (the same principle
  `lib/CLAUDE.md` already states for routing and overload handling) was judged the safer default here.
  Consequence: only `Transfer-Encoding: chunked` (exactly, case-insensitive, OWS-tolerant) is accepted;
  everything else - `gzip` alone, `gzip, chunked`, `chunked, gzip`, `chunked, chunked` (a duplicate token
  is still "not exactly one" by this rule), or the same coding list split across two separate duplicate
  `Transfer-Encoding` header instances (checked per-instance, so neither half alone is the sole token
  "chunked") - is rejected.
- **New content_length sentinel, `-3` (`req.content_length` only, not to be confused with the already-
  retired `-3` in `parse_http_request`'s own top-level return code, a different code space entirely -
  documented explicitly in both `http_parser.h` and `lib/CLAUDE.md` to head off exactly that confusion).**
  `compute_content_length_and_chunked` returns `-3` when `te_unsupported` is set, checked before the
  pre-existing `chunked+Content-Length -> -1` conflict check (an unsupported/ambiguous encoding is
  reported as such regardless of what Content-Length says). This plumbs through for free everywhere
  `content_length < 0` was already handled specially: `request_head_is_complete` already reports
  "complete" for any negative `content_length` (no change needed), `reject_if_over_body_limit` (S4)
  already skips its own check for any negative `content_length` (no change needed), and
  `parse_http_request_from_head`'s existing `if (head->content_length < 0) { req->content_length =
  head->content_length; return -1; }` branch (predates this fix) already copies the sentinel through to
  `req.content_length` and returns the generic `-1`. The only new code needed downstream was in
  `connection.c`'s status-mapping ternary: `req.content_length == -3 ? 501 : 400`, alongside the
  pre-existing `== -2 ? 413`.

**Deliberately not done:** `improvements.md`'s fix sketch mentions "400" as an alternative to 501 for a
bad `Transfer-Encoding` ("reject ... with 501/400"). 501 was chosen exclusively, not both/either, because
RFC 9112 §6.1 specifically prescribes 501 for "a transfer coding it does not understand," which is exactly
this case - the request is not syntactically malformed (400's usual meaning here), the server simply
doesn't implement the coding named. A bare `\n` line ending remains 400 (genuinely malformed framing, no
ambiguity about which code fits).

**Tests and results.**

- Unit tests added to `tests/test_http_hardening.c` (registered in `main`):
  - `test_bare_lf_rejected` - three shapes (bare `\n` ending the request line, a header line, and the
    final blank line) each asserted through `request_framing` (`-1`, `header_len == 0`),
    `request_is_complete` (`1`, immediate rejection not "need more"), and `parse_http_request` (`-1`); an
    ordinary fully-CRLF request is confirmed unaffected.
  - `test_transfer_encoding_token_matching` - a table of nine `Transfer-Encoding` values covering: plain
    `chunked` (and case/OWS variants) accepted; `xchunked`/`chunkedx` (substring, not token) rejected;
    `gzip` alone, `gzip, chunked`, `chunked, gzip`, and `chunked, chunked` all rejected (`-3`); plus a
    separate case for the same coding split across two duplicate `Transfer-Encoding` header instances
    (`gzip` on one line, `chunked` on another) confirming neither line alone satisfies "sole token
    chunked"; and an end-to-end `parse_http_request` case confirming `-3` surfaces as `req.content_length
    == -3` with a top-level return of `-1`, the same generic plumbing S4/S8 already established for their
    own sentinels.
  - Updated the pre-existing `test_request_framing`'s `"Transfer-Encoding: gzip, chunked"` case, which
    previously asserted this was accepted as chunked framing (`chunked == 1`) - the exact previously-wrong
    behavior S11 targets - to assert the new, correct rejection (`-3`, `chunked == 0`) instead, with a
    comment explaining why the expectation changed rather than silently flipping the assertion.
- Unit tests added to `tests/test_connection.c` (registered in `main`), end-to-end through the real
  `handle_readable` path rather than the pure parser functions above:
  - `test_handle_readable_bare_lf_400` - `GET / HTTP/1.1\nHost: x\r\n\r\n` (bare-`\n`-terminated request
    line) gets an explicit `400 Bad Request` and the connection is closed.
  - `test_handle_readable_unsupported_transfer_encoding_501` - `Transfer-Encoding: gzip, chunked` gets an
    explicit `501 Not Implemented` and the connection is closed, rather than being silently accepted and
    mishandled as plain chunked framing.
- `make test`: all 13 suites pass (test_http_hardening: 2 new cases + 1 updated; test_connection: 2 new
  cases).
- `make SANITIZE=1 BUILD_DIR=build-asan test`: all 13 suites pass clean under ASan + UBSan.
- `make fuzz` (1,000,000 iterations): clean (118,469 parsed, 784,307 complete - both figures move run to
  run depending on what the random mutator happens to generate and are not meaningful to compare directly
  against a prior run's numbers elsewhere in this document).
- `make check-docs`: passes (126 engine functions - no public API changed; `is_sole_token`/`has_bare_lf`
  are file-local statics).
- Compiled `lib/http_parser.c` and `lib/connection.c` directly with the project's `-Wall -Wextra -std=c11
  -O2` flags: no new warnings.
- Verified every existing test that constructs a `Transfer-Encoding: chunked` request (`test_http_parser.c`,
  `test_connection.c`'s chunked-body suite) still passes unchanged - all of them already used the single
  exact token, so the stricter matching doesn't disturb any legitimate existing coverage; only the one
  `test_request_framing` case using the "gzip, chunked" shape needed updating (above), and it was updating
  a previously-wrong expectation, not losing coverage.

**Status:** Fixed. `scan_framing`'s copy of the fix (backing the public `extract_content_length`/
`request_has_chunked_encoding` accessors) applies the same token-exact matching for consistency between
the two implementations, but - being a boolean-only accessor with no HTTP status code to report - has no
`-3`/501 concept of its own; it simply returns `0` ("not chunked") for anything that isn't the sole token
"chunked", same as it already did for values that plainly weren't chunked at all (e.g. `"gzip"` alone,
already correctly `0` before this fix). No hot-path cost beyond what framing already computed: both new
helpers run once per request, only over bytes already being scanned for other reasons (the header block,
already located; a `Transfer-Encoding` value, already being read to decide `chunked_out`).

## S12 · Startup allocations unchecked; library calls `exit()`

**Date completed.** 2026-09-23.

**How it was completed.**

Two independent problems in `improvements.md`'s S12, both in route registration (`lib/router.c`) plus
one in socket setup (`lib/connection.c`), fixed separately since they don't share code:

- **`fill_route` now rejects an over-long `method`/`path` instead of truncating it (`lib/router.c`).**
  The old `strncpy(route->path, path, sizeof(route->path) - 1)` silently cut a pattern over 255 chars
  down to whatever fit, registering a shorter route than the caller asked for with no signal anything
  was wrong - a request whose path happened to match that truncated prefix would hit a route the caller
  never actually registered. `fill_route` changed from `void` to `int`: it now checks
  `strlen(method) >= sizeof(route->method)` and `strlen(path) >= sizeof(route->path)` up front and
  returns `-1` (route left untouched, a `stderr` message printed) before touching either field, `0` on
  success. All three callers (`app_add_route_mw`, `router_add_route_mw`, `app_serve_static`) check the
  return and skip registering rather than proceeding with a corrupted route: `app_add_route_mw` frees the
  `malloc`'d `Route` it no longer needs; `router_add_route_mw` (routes live in a fixed
  `Route[MAX_ROUTER_ROUTES]` array, not malloc'd) simply leaves the slot unfilled and does not increment
  `route_count`, the same "reject, don't consume a slot" convention it already used for the
  `MAX_ROUTER_ROUTES`-exceeded case just above it.
- **Every allocation in the Patricia tree build path is now checked (`create_patricia_node`,
  `tree_insert`, `lib/router.c`).** `create_patricia_node`'s `calloc`/`malloc` were unchecked (a failure
  handed the caller a node with a dangling or missing `prefix`); it now returns `NULL` on either failing,
  freeing the partially-built node first if the second allocation (`prefix`) is the one that fails.
  `tree_insert`'s own root-node creation, every per-segment `create_patricia_node` call (static child,
  `:name` child, `*` child, catch-all child), and the `children` array `realloc` are each checked; any
  failure prints one `stderr` message, frees the `Route` being inserted (it was never linked into the
  tree), and returns without corrupting `current->children`/`child_count` - the `realloc` result lands in
  a local `new_children` first and only replaces `current->children` once confirmed non-`NULL`, so a
  failed `realloc` (which leaves the original block untouched) doesn't leak it or write through a `NULL`
  pointer via the following `memmove`. Any tree structure already linked in before the failing allocation
  is left in place deliberately, not unwound: a `PatriciaNode` with `route == NULL` is already a normal,
  valid internal node (used by every route that shares that path prefix), so a partially-built path
  costs nothing and helps the next successful insert under the same prefix reuse it.
  `app_add_route_mw` and `app_serve_static`'s own `malloc(sizeof(Route))` (the `Route` handed to
  `tree_insert`) are checked the same way, printing a message and returning rather than passing a `NULL`
  route pointer downstream.
- **`create_server_socket` (`lib/connection.c`) returns `-1` on failure instead of `perror()`+`exit()`.**
  Every failing step (`socket`, both `setsockopt`s except the best-effort `SO_REUSEPORT`, `bind`,
  `listen`, and now `set_nonblocking` too - previously not even checked) already closes the fd it opened
  (steps after the first) and returns `-1`, `perror`ing the specific syscall that failed exactly as
  before - only the process-killing `exit(EXIT_FAILURE)` calls were removed. This makes the function
  reusable as a plain library building block instead of one that can unilaterally terminate whatever
  process links it in. The two call sites now decide for themselves whether a failure here is fatal, and
  both still choose to `exit(EXIT_FAILURE)` after printing their own context-specific message - same
  externally observable behavior, decision now made at the outer boundary rather than forced deep inside
  socket setup, matching the convention `event_loop_init` already used (see `lib/CLAUDE.md`, "Return
  conventions"/"Workers and fork"):
  - `app_listen_worker` (`connection.c`): checks `app->server_fd < 0` right after the call (only reached
    when `!app->accept_via_fd_passing`, i.e. not the `CEXPRESS_SINGLE_ACCEPTOR` fd-passing worker path,
    which never calls this function at all) and exits with a one-line message naming the port.
  - `cluster_listen` (`cluster.c`): its pre-existing S7 preflight bind check (validate the port once,
    before forking anyone, so a fatal misconfiguration doesn't produce a respawn storm - see the S7
    section above) relied on `create_server_socket`'s own `exit()` to do the actual stopping; it now
    checks the return itself (both the `CEXPRESS_SINGLE_ACCEPTOR` branch, which keeps the fd open for the
    cluster's lifetime, and the plain preflight-only branch, which discards it) and exits the same way.
  `event_loop_init` was *not* changed - it already returns an error code (`0`/`-1`); the "so the
  application decides" half of S12's fix sketch was already satisfied for it, `app_listen_worker` already
  chooses to `perror`+`exit()` on its failure today, and `lib/CLAUDE.md` documents this as the intended,
  permanent contract ("there is no runtime fallback to epoll"), not a gap. `cluster_listen`'s own
  restart-budget-exhausted `exit()` (S7, unrelated allocation-free code path near the end of the
  function) and `app_init`'s `calloc`/`malloc` `exit()`s (the connection table and the M1 shared arena -
  the server cannot run at all without either, and both already document this as the same intentional
  convention) are both out of scope for this fix and unchanged - `improvements.md`'s S12 entry names only
  `app_add_route_mw`/`create_patricia_node`/`realloc`/`fill_route`/`create_server_socket` specifically.

**Deliberately not done:** turning `app_listen`/`app_listen_worker`/`cluster_listen` themselves into
functions that return an error code all the way back to `main()` - `improvements.md`'s own S7 fix record
(the `cluster_listen` restart-budget-exhausted comment) already calls this "a larger, separate change";
S12 only asked for the two named unchecked-`exit()` sources to stop forcing that decision internally,
which is what this fix does, not a change to the public `app_listen` contract (still `void`, unchanged in
every example/doc/test).

**Tests and results.**

- Unit tests added to `tests/test_router.c` (registered in `main`):
  - `test_app_add_route_rejects_overlong_path` - a path of exactly 255 chars (fits `Route.path`) is
    accepted and matches normally; a path of 257 chars is rejected, and confirms the specific old bug
    this targets doesn't recur: the rejected route's *truncated first-255-chars prefix* does not match
    anything either (it was never silently registered under a shorter name).
  - `test_router_add_route_rejects_overlong_path` - the `Router` (fixed-array) path: an over-long pattern
    passed to `router_get` leaves `router.route_count` at `0`, confirming the slot is left uncounted
    rather than filled with a truncated pattern.
- No new allocation-failure tests were added: none of `malloc`/`calloc`/`realloc` in this codebase can be
  made to fail deterministically without a fault-injection allocator this project doesn't have (the
  existing convention throughout - `app_init`, `connection_create`, elsewhere - is the same: checked, not
  independently tested for the OOM branch itself). The code paths were instead verified by inspection and
  by the fact every existing route-registration test (`test_router.c`, `test_cookbook.c`, the demo) still
  registers and matches routes correctly through the now-checked path.
- `make test`: all 13 suites pass, including `test_cluster.c`'s
  `test_cluster_master_exits_fast_when_port_is_taken` (S7's own regression, unchanged - it asserts the
  master exits non-zero in under 2 seconds when the port is already bound, which now happens through
  `cluster_listen`'s own explicit exit rather than `create_server_socket`'s former internal one; the
  observable behavior is identical, confirmed by this suite still passing without modification) and
  `test_cluster_master_exits_after_restart_budget_exceeded` (S7's other regression, exercising the
  separate, unrelated `cluster_listen` `exit()` at the end of the function, also unchanged).
- `make SANITIZE=1 BUILD_DIR=build-asan test`: all 13 suites pass clean under ASan + UBSan.
- `make fuzz` (1,000,000 iterations): clean (118,469 parsed, 784,307 complete).
- `make check-docs`: passes (126 engine functions - no public API changed; `fill_route` stayed a
  file-local `static`, `create_server_socket` was already declared in `connection.h` and its signature is
  unchanged, only its failure behavior).

**Status:** Fixed. Every allocation named in `improvements.md`'s S12 entry is now checked, an over-long
route pattern is rejected rather than silently registering a different, shorter route, and
`create_server_socket` no longer forces the calling process to exit - the two composing functions that
call it (`app_listen_worker`, `cluster_listen`) make that call themselves, at the same points and with the
same externally observable outcome as before.

---

## P8 · Chunked bodies are re-scanned from the start on every `recv`

**Date completed.** 2026-09-23.

**How it was completed.**

The fix `improvements.md` suggested: keep the scan position and decoded length per connection, and resume
from there.

- **`lib/app_types.h`: new `ChunkScanState`** `{pos, decoded_len, trailer_from}`. All three are offsets from
  the body start, not pointers, so they stay valid when `grow_in_buf` reallocs `in_buf` between reads.
  Zeroed means "start of body".
- **`lib/http_parser.c`: new `chunked_body_scan_resume(body, available, max, state, &decoded_len)`.** Same
  loop and same result codes as before. `state->pos` moves forward only past a chunk whose size line, data
  and trailing CRLF have all arrived and been checked, so every `0` (need more) return leaves it at a size
  line that must be read again (at most `MAX_CHUNK_SIZE_LINE_LEN` bytes), never at body already scanned.
  After the last-chunk (`0`) line, the search for the trailer's `\r\n\r\n` restarts at `trailer_from`,
  which is set to 3 bytes before where the last search stopped. The 3 bytes catch a terminator split
  across two reads. Without this, a slowly sent trailer would still be scanned in quadratic time.
  `chunked_body_scan` is now a wrapper that runs the resume version from a zeroed state, so there is one
  scanner.
- **`request_head_is_complete` has a 4th parameter, `ChunkScanState *`.** `NULL` means scan from scratch,
  which `request_is_complete` and `tests/bench_hotpath.c` use.
- **`lib/connection.c`:** `handle_readable` passes `&conn->chunk_scan`. `Connection.chunk_scan` is zeroed
  by `connection_create`'s `calloc`, and `flush_connection` zeroes it in its keep-alive reset (next to
  `body_limit_checked`). A second request on the same connection therefore starts at its own body start.
- **Not changed:** when the request completes, `parse_http_request_from_head` still runs one from-scratch
  scan and then decodes. That is linear and happens once per request. It could reuse `decoded_len` from
  the state, but P8 did not need that. The separate cap on chunk count or framing overhead that
  `improvements.md` also mentioned was **not** added. With a linear scan, the worst case under the existing
  10 MiB raw wire cap costs about 14 ms of CPU. This is now noted in `lib/CLAUDE.md`'s known gaps.

**Tests and results.**

- `tests/test_http_parser.c` `test_chunked_body_scan_resume`: sends several bodies one byte at a time
  through one carried state. For each prefix it asserts the same verdict and `decoded_len` as a from-scratch
  `chunked_body_scan`. The bodies cover multiple chunks, extensions plus a multi-line trailer, many 1-byte
  chunks, a bad data CRLF, bad hex after a good chunk, and the cumulative size cap. It also checks that
  `pos`/`decoded_len` stop at the right chunk boundary. Two more checks: overwriting the bytes before `pos`
  with junk does not change the result, which shows the resume really skips them; and `trailer_from`
  advances while a long trailer is sent in pieces.
- `tests/test_connection.c` `test_handle_readable_chunked_scan_resumes_and_resets`: sends a chunked body
  over socketpair reads that split chunk data. It asserts that `conn->chunk_scan` advances only at chunk
  boundaries and that the response is `received 11 bytes`. It asserts that the state is zeroed after the
  keep-alive response. Then a second, shorter chunked request on the same connection must return
  `received 3 bytes`. A stale `pos` would point past that request's whole body, and it would never
  complete.
- `tests/fuzz_parser.c`: for every mutated input that frames as chunked, the fuzzer scans the body in two
  steps split at a random offset, carrying the state between them. It aborts if the result differs from a
  single from-scratch scan.
- `make test`: all 13 suites pass. `make SANITIZE=1 BUILD_DIR=build-asan test`: all pass with ASan and
  UBSan. `make fuzz`: 1,000,000 iterations clean. `make check-docs`: ok (127 engine functions;
  `chunked_body_scan_resume` added to `lib/API.md`).
- **Benchmark** (scratch program outside the repo, Apple M3 Pro, gcc-16 -O2). It matches `improvements.md`'s
  attack: 1-byte chunks (`1\r\nx\r\n`) delivered 1 KiB per simulated `recv`, and it runs
  `parse_request_head` plus `request_head_is_complete` per read, as `handle_readable` does.
  "Scratch" passes `NULL`, which is the old behavior. "Resume" passes a carried state.

  | Raw body | Reads | Scratch (old) | Resume (P8) |
  |---|---|---|---|
  | 1 MiB | 1,026 | 709.5 ms | 1.4 ms |
  | 2 MiB | 2,050 | 2,762.0 ms | 2.8 ms |
  | 10 MiB | 10,242 | ~71 s (extrapolated: 3.9x per doubling, so quadratic) | 14.2 ms |

  **MEASURED:** the 10 MiB worst case drops from about 71 s of one worker's CPU (`improvements.md`
  extrapolated about 75 s) to 14.2 ms. `improvements.md` PROJECTED about 15 ms. The resume column includes
  the per-read header parse.

**Status:** Fixed as scoped. The optional cap on chunk count or framing overhead is still open (see above).
