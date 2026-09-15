# lib/ — engine internals

## Architecture
Single-threaded, non-blocking event loop on kqueue (`connection.c: app_listen`). One
`App` owns the listening socket, the kqueue fd, a fixed route table (`MAX_ROUTES`), and
a `Connection *` slot per possible fd (`app->connections[MAX_CONNECTIONS]`, indexed
directly by fd value — this bounds the server to fds below `MAX_CONNECTIONS`).

Data flow per request: `handle_readable` (I/O) accumulates bytes into
`conn->in_buf` → `request_is_complete` (pure, `httpParser.c`) checks the buffer
without mutating it → `parse_http_request` (pure) fills a `Request` → `match_route`
(pure, `router.c`) looks up a `Handler` → the handler calls `res_send`/`res_json`
(`response.c`), which only builds bytes into `conn->out_buf` and never touches the
socket → `flush_connection` (I/O) is what actually writes. This split exists so the
parsing/routing/response-building layer stays pure and unit-testable independent of
sockets — see `lib/json/json_test.c` for the pattern; `httpParser`/`router`/`response`
have no equivalent tests yet (tracked in `../pending.txt`).

## Deny-by-default routing
`match_route` returns `NULL` on no match; `handle_readable` turns that into a 404.
There is no fallback/wildcard handler, so an unregistered path or method is always
rejected rather than silently served.

## Request size limits
The whole request (headers + body) shares one `BUF_SIZE` (8192-byte) buffer,
`conn->in_buf`. Two independent guards enforce this hard limit instead of letting a
malformed or hostile client stall a connection slot forever:
- `extract_content_length` (`httpParser.c`) returns `-1` for a negative or
  oversized (`> BUF_SIZE - 1`) `Content-Length`; `parse_http_request` turns that into
  a 400 rather than trusting or truncating it.
- `handle_readable` (`connection.c`) detects when `in_buf` fills up
  (`in_len >= BUF_SIZE - 1`) without ever producing a complete request — headers
  with no `\r\n\r\n` terminator, or a body that can never arrive — and responds 431 +
  closes instead of leaving the connection open indefinitely (the Slowloris-shaped
  gap noted in `../pending.txt`).

There is still no idle/read *timeout* for a slow client trickling bytes in below
these limits — only the hard buffer-size cutoff above is enforced.

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
- `Request` and the per-connection `in_buf` are fixed-size (`appTypes.h`), never
  heap-allocated.

## Const correctness
`Handler` (`appTypes.h`) takes `const Request *`: routing (`match_path`/`match_route`)
is the only code that mutates a `Request` (filling in path params before dispatch);
once a handler runs, the request is read-only for the rest of its lifetime.
