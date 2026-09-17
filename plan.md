# Pending Work — Complexity & Effort Estimate

Source: `pending.txt`. Only items **not** marked `[DONE]` are listed. Estimates
are rough (order-of-magnitude), assuming Claude-assisted implementation
following this repo's CLAUDE.md rules (dedicated header types, `const`
correctness, banned unsafe functions, tests required, nested CLAUDE.md updates,
1,000-line file cap).

| # | Task | Section | Complexity | Est. time | Est. tokens | Notes |
|---|------|---------|------------|-----------|-------------|-------|
| 1 | ~~Chunked `Transfer-Encoding` support (request body dechunking)~~ | 3 / 6 (listed twice) | Medium-High | 1–1.5 hr | ~40k–70k | **Done.** New pure parser (chunk-size lines, trailers, terminating `0\r\n\r\n`), wired into `http_parser.c`/`connection.c` buffering, rejects malformed/oversized chunks and the Transfer-Encoding+Content-Length smuggling shape (400), tests + `lib/CLAUDE.md` updated. |
| 2 | Streaming / chunked **response** support | 4 | High | 2–4 hr | ~80k–150k | Currently `Handler` is fully synchronous and materializes the whole body. True streaming needs an incremental write API and touches `response.c`, `connection.c` event loop, and likely the `Handler` signature — largest scope item outside concurrency work. |
| 3 | Static file serving | 4 | Medium | 1–2 hr | ~50k–90k | New module: safe path resolution (no traversal outside root), deny-by-default, MIME-type table, file read + send. Should reuse existing 403/404 conventions. |
| 4 | `res_redirect()` helper | 4 | Low | 15–30 min | ~8k–15k | Small helper: set `Location` header + 3xx status via existing `res_set_header`/status machinery. Straightforward, low risk. |
| 5 | Cookie helper (listed as missing) | 4 | N/A — likely stale | ~0 (doc fix only) | ~2k | `res_set_cookie`/`res_clear_cookie`/`req_get_cookie` already exist per section 3's `[DONE]` cookies entry. This line in the Response section appears to predate that work; just needs `pending.txt` cleanup, not code. |
| 6 | Expand `status_text()` beyond 11 codes | 4 | Low | 15–20 min | ~5k–10k | Mechanical: add more status-code → reason-phrase mappings. |
| 7 | epoll/IOCP backend (portability beyond kqueue) | 5 | Very High | 1–2+ days | ~200k–400k | Requires abstracting the event-loop interface behind kqueue today, then a parallel epoll (Linux) implementation; IOCP (Windows) is a materially different async model and would likely be its own follow-up. Touches `connection.c` and the accept/read/write/timer paths broadly. |
| 8 | Multi-threaded / multi-process worker model | 5 | Very High | 1–3 days | ~250k–500k | Fundamental concurrency change (e.g. `SO_REUSEPORT` + one event loop per worker, or a thread pool with shared state/locking). High risk of subtle races; needs careful design before implementation. |
| 9 | Replace fd-indexed `connections[MAX_CONNECTIONS]` array | 5 | Medium | 1–2 hr | ~40k–70k | Swap fixed array (assumes `fd < 16384`) for a dynamic structure (hash map or growable table) keyed by fd; moderate blast radius since every lookup site needs updating. |
| 10 | Graceful shutdown (SIGTERM draining) | 5 | Medium | 1–2 hr | ~40k–60k | Signal handler + event-loop cooperation: stop accepting, let in-flight requests finish (with a timeout), then close cleanly. |
| 11 | TLS/HTTPS support | 6 | Very High | 2–4+ days | ~300k–600k | Integrate OpenSSL/LibreSSL non-blocking handshake into the kqueue event loop, cert/key config, error handling for partial TLS reads/writes. Largest and riskiest single item. |

## Rough totals
- **Quick wins** (#4, #5, #6): under an hour combined, mostly mechanical.
- **Medium items** (#1, #3, #9, #10): a solid day of focused work in total.
- **Large architectural items** (#2, #7, #8, #11): each is multi-day and would
  benefit from its own design discussion before implementation — these are
  the ones most likely to need splitting into sub-tasks and separate PRs to
  respect the 1,000-line file cap and keep changes reviewable.

## Suggested order (per `pending.txt` + this estimate)
1. ~~Cleanup: fix the stale "no cookie helper" line in `pending.txt` (#5).~~ Done.
2. ~~Quick wins: `res_redirect`, expanded `status_text()` (#4, #6).~~ Done.
3. ~~Chunked request support (#1) — needed before real interop with many HTTP clients.~~ Done.
4. Static file serving (#3) and connection-table refactor (#9) — independent, medium-sized.
5. Graceful shutdown (#10) — moderate, improves operability before tackling concurrency.
6. Design spikes for the big three: streaming responses (#2), TLS (#11), and
   the concurrency/portability model (#7, #8) — these interact (e.g. TLS and
   streaming both touch the write path; a worker model changes what "the
   event loop" even means), so they're best scoped together rather than
   picked off independently.
