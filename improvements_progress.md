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
