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
| 4 | ~~`res_redirect()` helper~~ | 4 | Low | 15–30 min | ~8k–15k | **Done.** Sets `Location` header and 3xx status with short redirect text body. Tested in `test_response.c`. |
| 5 | ~~Cookie helper~~ | 4 | N/A | ~0 | ~2k | **Done.** `res_set_cookie`, `res_clear_cookie`, and `req_get_cookie` with full SameSite/Max-Age/Secure/HttpOnly options. |
| 6 | ~~Expand `status_text()` beyond 11 codes~~ | 4 | Low | 15–20 min | ~5k–10k | **Done.** Covers 27 standard HTTP status codes in `http_parser.c`. |
| 7 | ~~epoll/IOCP backend (portability beyond kqueue)~~ | 5 | Very High | 1–2+ days | ~200k–400k | **Done.** Abstracted event loop interface (`lib/event_loop.h`), native BSD/macOS `kqueue` backend (`lib/event_loop_kqueue.c`), and native Linux `epoll` backend (`lib/event_loop_epoll.c`) using `epoll_create1`, `epoll_ctl`, `timerfd`, and `signalfd`. Tested via unit tests (`test_event_loop`, `test_connection`), macOS `epoll-shim` test harness (`make test_epoll`), native Alpine Linux Docker container, and `wrk` load test (>61k req/sec inside Docker). |
| 8 | ~~Multi-threaded / multi-process worker model~~ | 5 | Very High | 1–3 days | ~250k–500k | **Done.** Multi-process worker model (`lib/cluster.c/h`) with kernel-level `SO_REUSEPORT` socket load balancing. Master supervisor reaps child processes (`waitpid`), automatically respawns unexpectedly terminated workers, and coordinates graceful cluster drain on `SIGINT`/`SIGTERM`. Supports `WORKERS=auto` or `N`. Tested via unit tests (`test_cluster.c`), macOS host, and Linux Docker container. |
| 9 | ~~Replace fd-indexed `connections[MAX_CONNECTIONS]` array~~ | 5 | Medium | 1–2 hr | ~40k–70k | **Done.** `App.connections` is now a heap-allocated, growable `Connection **` (`app_types.h`) starting at `INITIAL_CONNECTION_TABLE_CAP` (1024) and grown by the new `ensure_connection_capacity` (`connection.c`) as needed — no more fixed ceiling, bounded only by the process's own `RLIMIT_NOFILE`. New `app_destroy` (`connection.c/h`) is the documented match for `app_init`'s allocation. |
| 10 | ~~Graceful shutdown (SIGTERM/SIGINT draining)~~ | 5 | Medium | 1–2 hr | ~40k–60k | **Done.** `app_listen` (kqueue `EVFILT_SIGNAL` for SIGINT/SIGTERM with default dispositions ignored) + `app_stop`: stops accepting, immediately drops idle keep-alive connections, sets `Connection: close` on in-flight requests, arms 5s oneshot timeout (`SHUTDOWN_TIMEOUT_SECONDS`), drains cleanly, and returns to `main.c` which calls `app_destroy(&app)` and exits 0. |
| 11 | TLS/HTTPS support | 6 | Very High | 2–4+ days | ~300k–600k | Integrate OpenSSL/LibreSSL non-blocking handshake into the kqueue event loop, cert/key config, error handling for partial TLS reads/writes. Largest and riskiest single item. |

## Rough totals
- **Quick wins** (#4, #5, #6): under an hour combined, mostly mechanical. Done.
- **Medium items** (#1, #3, #9, #10): all done (#1 chunked requests, #3 static files, #9 connection table, #10 graceful shutdown).
- **Large architectural items** (#2, #7, #8, #11): #2 (Streaming & chunked responses), #7 (Linux `epoll` portability), and #8 (Multi-worker concurrency) are Done! Only #11 (TLS/HTTPS) remains.

## Suggested order (per `pending.txt` + this estimate)
1. ~~Cleanup: fix the stale "no cookie helper" line in `pending.txt` (#5).~~ Done.
2. ~~Quick wins: `res_redirect`, expanded `status_text()` (#4, #6).~~ Done.
3. ~~Chunked request support (#1) — needed before real interop with many HTTP clients.~~ Done.
4. ~~Static file serving (#3) and connection-table refactor (#9) — independent, medium-sized.~~ Done.
5. ~~Graceful shutdown (#10) — moderate, improves operability before tackling concurrency.~~ Done.
6. ~~Streaming & chunked responses (#2) — incremental writes, trailers, and bounded file streaming.~~ Done.
7. ~~Linux `epoll` Portability (#7)~~: Native Linux & Docker support without third-party wrappers. Done.
8. ~~Multi-Worker Concurrency (#8)~~: `SO_REUSEPORT` master-worker supervisor scaling across CPU cores. Done.
9. Remaining Milestone:
   - **TLS / HTTPS (#11)**: Non-blocking OpenSSL/LibreSSL handshake and encrypted framing.

