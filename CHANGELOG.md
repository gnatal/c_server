# Changelog

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
