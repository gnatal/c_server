# Pending Work — Complexity & Effort Estimate

Source: `pending.txt`. Only items **not** marked `[DONE]` are listed. Estimates
are rough (order-of-magnitude), assuming Claude-assisted implementation
following this repo's CLAUDE.md rules (dedicated header types, `const`
correctness, banned unsafe functions, tests required, nested CLAUDE.md updates,
1,000-line file cap).

| # | Task | Section | Complexity | Est. time | Est. tokens | Notes |
|---|------|---------|------------|-----------|-------------|-------|
| 1 | ~~Chunked `Transfer-Encoding` support (request body dechunking)~~ | 3 / 6 (listed twice) | Medium-High | 1–1.5 hr | ~40k–70k | **Done.** New pure parser (chunk-size lines, trailers, terminating `0\r\n\r\n`), wired into `http_parser.c`/`connection.c` buffering, rejects malformed/oversized chunks and the Transfer-Encoding+Content-Length smuggling shape (400), tests + `lib/CLAUDE.md` updated. |
| 2 | ~~Streaming / chunked **response** support~~ | 4 | High | 2–4 hr | ~80k–150k | **Done.** Procedural chunked responses (`res_write`/`res_end`) with dynamic buffer growth up to `MAX_BODY_SIZE` (10MB), RFC 7230 chunked trailers (`res_set_trailer`), HEAD chunk suppression, and bounded 16KB file streaming (`res_send_file`) in `flush_connection` with 64KB cooperative yielding per turn. Tested in `test_response` and `test_connection`; demo routes `/stream` and `/download`. |
| 3 | ~~Static file serving~~ | 4 | Medium | 1–2 hr | ~50k–90k | **Done.** New `lib/static.c/h`: pure path resolution (`static_resolve_relative_path`, rejects `..`) plus a `realpath()`-based containment check against symlink escapes, MIME-type table, `app_serve_static` (`router.h`) registers a mount as a `Route` rather than a `Handler`. 403/404/500 per the existing conventions; new `res_send_bytes` for NUL-safe binary responses. |
| 4 | `res_redirect()` helper | 4 | Low | 15–30 min | ~8k–15k | Small helper: set `Location` header + 3xx status via existing `res_set_header`/status machinery. Straightforward, low risk. |
| 5 | Cookie helper (listed as missing) | 4 | N/A — likely stale | ~0 (doc fix only) | ~2k | `res_set_cookie`/`res_clear_cookie`/`req_get_cookie` already exist per section 3's `[DONE]` cookies entry. This line in the Response section appears to predate that work; just needs `pending.txt` cleanup, not code. |
| 6 | Expand `status_text()` beyond 11 codes | 4 | Low | 15–20 min | ~5k–10k | Mechanical: add more status-code → reason-phrase mappings. |
| 7 | epoll/IOCP backend (portability beyond kqueue) | 5 | Very High | 1–2+ days | ~200k–400k | Requires abstracting the event-loop interface behind kqueue today, then a parallel epoll (Linux) implementation; IOCP (Windows) is a materially different async model and would likely be its own follow-up. Touches `connection.c` and the accept/read/write/timer paths broadly. |
| 8 | Multi-threaded / multi-process worker model | 5 | Very High | 1–3 days | ~250k–500k | Fundamental concurrency change (e.g. `SO_REUSEPORT` + one event loop per worker, or a thread pool with shared state/locking). High risk of subtle races; needs careful design before implementation. |
| 9 | ~~Replace fd-indexed `connections[MAX_CONNECTIONS]` array~~ | 5 | Medium | 1–2 hr | ~40k–70k | **Done.** `App.connections` is now a heap-allocated, growable `Connection **` (`app_types.h`) starting at `INITIAL_CONNECTION_TABLE_CAP` (1024) and grown by the new `ensure_connection_capacity` (`connection.c`) as needed — no more fixed ceiling, bounded only by the process's own `RLIMIT_NOFILE`. New `app_destroy` (`connection.c/h`) is the documented match for `app_init`'s allocation. |
| 10 | ~~Graceful shutdown (SIGTERM/SIGINT draining)~~ | 5 | Medium | 1–2 hr | ~40k–60k | **Done.** `app_listen` (kqueue `EVFILT_SIGNAL` for SIGINT/SIGTERM with default dispositions ignored) + `app_stop`: stops accepting, immediately drops idle keep-alive connections, sets `Connection: close` on in-flight requests, arms 5s oneshot timeout (`SHUTDOWN_TIMEOUT_SECONDS`), drains cleanly, and returns to `main.c` which calls `app_destroy(&app)` and exits 0. |
| 11 | TLS/HTTPS support | 6 | Very High | 2–4+ days | ~300k–600k | Integrate OpenSSL/LibreSSL non-blocking handshake into the kqueue event loop, cert/key config, error handling for partial TLS reads/writes. Largest and riskiest single item. |

## Rough totals
- **Quick wins** (#4, #5, #6): under an hour combined, mostly mechanical. Done.
- **Medium items** (#1, #3, #9, #10): all done (#1 chunked requests, #3 static files, #9 connection table, #10 graceful shutdown).
- **Large architectural items** (#2, #7, #8, #11): each is multi-day and would
  benefit from its own design discussion before implementation — these are
  the ones most likely to need splitting into sub-tasks and separate PRs to
  respect the 1,000-line file cap and keep changes reviewable.

## Suggested order (per `pending.txt` + this estimate)
1. ~~Cleanup: fix the stale "no cookie helper" line in `pending.txt` (#5).~~ Done.
2. ~~Quick wins: `res_redirect`, expanded `status_text()` (#4, #6).~~ Done.
3. ~~Chunked request support (#1) — needed before real interop with many HTTP clients.~~ Done.
4. ~~Static file serving (#3) and connection-table refactor (#9) — independent, medium-sized.~~ Done.
5. ~~Graceful shutdown (#10) — moderate, improves operability before tackling concurrency.~~ Done.
6. Design spikes for the big three: streaming responses (#2), TLS (#11), and
   the concurrency/portability model (#7, #8) — these interact (e.g. TLS and
   streaming both touch the write path; a worker model changes what "the
   event loop" even means), so they're best scoped together rather than
   picked off independently.
