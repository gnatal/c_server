# Changelog

## Unreleased

- **`Expect` is now treated as the comma-separated list RFC 9110 §10.1.1 defines** (`#expectation`), not as a
  single value: classification is per member, OWS-trimmed and case-insensitive, and token-exact like
  `Transfer-Encoding` (so `X-Expect` and `100-continuex` are not `100-continue`).
- **A member other than `100-continue` is refused with `417 Expectation Failed`** as soon as the head is parsed,
  before the body is read or invited, and the connection is closed. Previously such an `Expect` field was ignored,
  which left the client holding back a body it had been told to send, and made `Expect: 100-continue, foo` get no
  interim response at all. HTTP/1.0 expectations stay ignored, an empty field names no expectation, and an invalid
  framing is still answered `400`/`413`/`501` by the parse path rather than masked by a `417`.
- **An empty `res_write` no longer ends a chunked body early.** `res_write(res, "", 0)` used to frame a zero-size
  chunk, which is the last-chunk marker: the client saw the body end there, and every later chunk, `res_end`'s
  terminator included, reached a keep-alive client as the start of a bogus next response. Once the head is committed,
  `len == 0` now writes nothing whatever `data` is (the first call still commits the head), as `stream_write` already
  did. `res_write(res, NULL, n)` with `n > 0` now returns `-1` with nothing written, instead of a size line with no
  bytes behind it.

## v0.1.0 (2026-09-27)

First tagged release.

### License

**CExpress is MIT licensed from v0.1.0 on.** Earlier, untagged commits were published under GPL-3.0; the sole author
relicensed the project to MIT for this release. See [LICENSE](LICENSE).

### What is in it

- Express-style routing (`app_get`, `app_post`, ..., path parameters, wildcards) on per-method Patricia trees, with
  sub-routers (`app_mount`), app-wide, prefix-scoped and per-route middleware, and a central error handler.
- Non-blocking HTTP/1.1 engine: kqueue on macOS/BSD, epoll on Linux (io_uring opt-in), keep-alive, pipelining with
  coalesced writes, and a multi-process `SO_REUSEPORT` cluster with graceful drain.
- Deferred responses (`res_defer` / `res_resume`) and application file descriptors (`app_watch_fd`), so handlers can
  wait on I/O such as a non-blocking database connection without blocking the worker.
- JSON via vendored yyjson over a per-worker arena; cookies, forms, multipart uploads, static files, chunked and
  producer-streamed responses.
- Hard input limits (8 KiB headers, 10 MiB bodies, per-worker buffered-memory budget) and deny-by-default routing.
- 22 test suites, run in CI on Linux (gcc) and macOS (clang); ASan/UBSan and a mutation fuzzer via `make`.

### Benchmarks

In TechEmpower's own harness (one laptop VM, so rankings within a run matter, not absolute numbers): 1st on updates
and multiple queries against actix, xitca-web and Round 23's database leaders; 1st on JSON and plaintext against actix,
axum, h2o and Fiber. Method and caveats: [techEmpV1.md](techEmpV1.md).

### Not included

TLS (terminate it at a proxy), HTTP/2, response compression, `Range`, WebSocket. See "Known Gaps" in the README.
