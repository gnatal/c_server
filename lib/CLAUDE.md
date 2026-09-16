# lib/ — engine internals

## Architecture
Single-threaded, non-blocking event loop on kqueue (`connection.c: app_listen`). One
`App` owns the listening socket, the kqueue fd, a fixed route table (`MAX_ROUTES`), and
a `Connection *` slot per possible fd (`app->connections[MAX_CONNECTIONS]`, indexed
directly by fd value — this bounds the server to fds below `MAX_CONNECTIONS`).

Data flow per request: `handle_readable` (I/O) accumulates bytes into
`conn->in_buf` → `request_is_complete` (pure, `http_parser.c`) checks the buffer
without mutating it → `parse_http_request` (pure) fills a `Request` → `match_route`
(pure, `router.c`) looks up a `Handler` (or returns `NULL`) → `dispatch`
(`middleware.c`) runs the middleware pipeline ending at that handler (or a 404) →
the handler calls `res_send`/`res_json` (`response.c`), which only builds bytes into
`conn->out_buf` and never touches the socket → `flush_connection` (I/O) is what
actually writes. This split exists so the parsing/routing/dispatch/response-building
layer stays pure and unit-testable independent of sockets:
- `tests/test_json.c` tests parsing, AST representation, and stringification.
- `tests/test_middleware.c` tests pipeline ordering, short-circuiting, 404 fallthrough, and error handlers.
- `tests/test_router.c` tests segment-by-segment tokenization, `:param` extraction, bounded param limits, and route table resolution.
- `tests/test_http_parser.c` tests pure request line, query, header, Content-Length boundary extraction, and keep-alive parsing.
- `tests/test_connection.c` tests non-blocking socket I/O, `handle_readable` state progression, keep-alive persistence, partial buffer reads, 400 Bad Request on malformed inputs, and 431 on header overflow via POSIX `socketpair(2)` with a dedicated `kqueue()` instance without opening live TCP ports.

`App` carries `ServerConfig config` (`app_types.h`), storing runtime parameters such as `config.port` (defaulting to `DEFAULT_PORT` in `app_init`).

## Middleware pipeline
`dispatch(app, route, req, res)` (`middleware.c`) builds one `MiddlewareChain` per
request and calls `chain_next` once to start it. The chain walks two arrays back to
back, then falls through to the handler:
1. the app's `middlewares[]` (registered via `app_use`, run in registration order,
   app-wide — every request passes through these, including ones that end up
   404ing), then
2. the matched route's own `middlewares[]` (registered via `app_get_mw`/
   `app_post_mw`/`app_add_route_mw`, stored directly on the `Route` — only requests
   that match that specific route run these), then
3. `route->handler` (`final_handler`) — or a default 404 if `route` is `NULL`.

`chain->index` counts continuously across both arrays: `chain_next` first drains
`middlewares[0..count)`, then `route_middlewares[0..route_middleware_count)`
(`chain->index - chain->count` gives the position within the route array), then
invokes `final_handler`. This is why route middleware always runs *after* every
app-wide middleware and *before* the handler, regardless of registration order.
- Every `Middleware` (app-wide or per-route) receives the live `MiddlewareChain *`
  and must either call `chain_next(chain)` to continue, write a response directly
  and *not* call `chain_next` (terminates the pipeline there), or call
  `chain_error(chain, status, message)`.
- `chain_error` is the C analogue of Express's `(err, req, res, next)`: it hands off
  to the app's single registered `ErrorHandler` (`app_use_error`), or — if none is
  registered — falls back to `res_status`+`res_send` with the given status/message
  directly. There is only one error handler per app (last `app_use_error` call wins),
  not a chain of them.
- Per-route middleware is scoped to that one route (a `Route`'s
  `Middleware[MAX_ROUTE_MIDDLEWARES]` + `middleware_count`, set at registration time
  by `app_add_route_mw` in `router.c`) — unlike `app_use`, it does not run for other
  routes or for 404s. There is still no *prefix*-scoped/mounted middleware (e.g. all
  of `/api/*`) — that's future work (`../pending.txt`, section 2 — sub-routers).
  Registering more than `MAX_ROUTE_MIDDLEWARES` for one route truncates the list
  (stderr warning) rather than overflowing the fixed array, same as
  `MAX_ROUTES`/`MAX_MIDDLEWARES` elsewhere.
- Because `chain_next` is a plain synchronous call (no deferred/async dispatch
  anywhere in this engine), a middleware that calls `chain_next(chain)` and then does
  more work afterward (e.g. logging) runs that code *after* the entire rest of the
  pipeline — including the handler — has already finished building the response, so
  `res->status` is final by then. See `app/middlewares.c: mw_logger`.
- Route `Handler`s themselves are not given a `MiddlewareChain *` and so cannot call
  `chain_next`/`chain_error` — they remain the terminal node of the pipeline exactly
  as before; only middleware (app-wide or per-route) participates in chaining.

## Deny-by-default routing
`match_route` returns `NULL` on no match; `handle_readable` turns that into a 404.
There is no fallback/wildcard handler, so an unregistered path or method is always
rejected rather than silently served.

## Request size limits
The whole request (headers + body) shares one `BUF_SIZE` (8192-byte) buffer,
`conn->in_buf`. Two independent guards enforce this hard limit instead of letting a
malformed or hostile client stall a connection slot forever:
- `extract_content_length` (`http_parser.c`) returns `-1` for a negative or
  oversized (`> BUF_SIZE - 1`) `Content-Length`; `parse_http_request` turns that into
  a 400 rather than trusting or truncating it.
- `handle_readable` (`connection.c`) detects when `in_buf` fills up
  (`in_len >= BUF_SIZE - 1`) without ever producing a complete request — headers
  with no `\r\n\r\n` terminator, or a body that can never arrive — and responds 431 +
  closes instead of leaving the connection open indefinitely (the Slowloris-shaped
  gap noted in `../pending.txt`).

There is still no idle/read *timeout* for a slow client trickling bytes in below
these limits — only the hard buffer-size cutoff above is enforced.

## Response headers
`Response` (`app_types.h`) carries a fixed `headers[MAX_RESPONSE_HEADERS]` array +
`header_count`, set via `res_set_header(res, name, value)` (`response.c`) ahead of
`res_send`/`res_json`. Rules enforced at set-time, not send-time:
- A second `res_set_header` call with the same name (case-insensitive) overwrites
  the stored value in place rather than appending a duplicate header line.
- `Content-Length` and `Connection` are rejected outright (logged to stderr, not
  stored) - both are computed by `send_with_content_type` itself (from `strlen(body)`
  and `conn->keep_alive`), so letting a handler override them would let the header
  and the actual bytes/socket state disagree.
- `Content-Type` *can* be set this way, and if present takes priority over the
  hardcoded default `res_send`/`res_json` pass to `send_with_content_type` - the
  custom value is used once and the default is suppressed, rather than emitting
  both.
- Once `header_count` reaches `MAX_RESPONSE_HEADERS`, further `res_set_header` calls
  are dropped (logged to stderr) rather than overflowing the fixed array - same
  bounded-array-over-sentinel reasoning as `route_count`/`middleware_count`/
  `param_count` elsewhere in this engine.
`send_with_content_type` builds the status line, the three built-in headers, and
every stored custom header into one fixed `RESPONSE_HEADER_BUF_SIZE` (8192-byte)
stack buffer via a running offset, checking each `snprintf` for truncation before
advancing it; on overflow (headers too large to fit) the connection is dropped
(`out_len = 0`, `keep_alive = 0`) the same way an `out_buf` allocation failure is
handled, rather than sending a truncated/malformed response.

## Memory lifecycle
- `Connection` (`connection_create`/`connection_close`): one `calloc` per accepted
  fd, freed exactly once in `connection_close`, which also deregisters the fd from
  kqueue and closes the socket first.
- `conn->out_buf` (`response.c: send_with_content_type`): one `malloc` per response,
  built by a handler via `res_send`/`res_json`. Freed in exactly one of two places —
  `flush_connection` once a keep-alive connection finishes writing it, or
  `connection_close` when the connection is torn down (error, `Connection: close`, or
  a write that never finished) — never both, since a connection only ever takes one
  of those two paths. If the `malloc` itself fails, `send_with_content_type` leaves
  `out_len` at 0 and marks the connection for close rather than writing through a
  NULL pointer.
- `Request` and the per-connection `in_buf` are fixed-size (`app_types.h`), never
  heap-allocated.

## Const correctness
`Handler` (`app_types.h`) takes `const Request *`: routing (`match_path`/`match_route`)
is the only code that mutates a `Request` (filling in path params before dispatch);
once a handler runs, the request is read-only for the rest of its lifetime.
