# tests/ — test harness & verification

## Architecture
Standalone unit and integration test suites compiled independently into `build/bin/test_*`. Tests run without third-party test frameworks using standard C `<assert.h>` assertions and return exit code 0 on complete pass:
- `test_json.c`: Pure data-structure tests covering JSON parsing, primitive tokens, nested arrays/objects, string escaping, and stringification round-trips.
- `test_middleware.c`: Middleware chain dispatching, synchronous execution ordering, termination by early response, 404 fallthrough, and error propagation via `chain_error`.
- `test_router.c`: Route registration limits (`MAX_ROUTES`), literal pattern matching, parameter tokenization (`:id`), and route param lookup (`req_get_param`).
- `test_http_parser.c`: Pure buffer parsing of HTTP/1.1 request lines, header scanning, Content-Length bounds enforcement, and keep-alive header determination.
- `test_connection.c`: Non-blocking socket I/O, `handle_readable` buffer progression, keep-alive connection reuse, partial buffer reads, and buffer overflow cutoff (431 Request Header Fields Too Large / 400 Bad Request) via POSIX `socketpair(2)` connected to an isolated `kqueue()` instance without binding to physical network ports.
- `test_response.c`: `res_set_header` behavior - custom headers appear in the built
  response, same-name overwrite (case-insensitive), `Content-Length`/`Connection`
  rejected as reserved, a custom `Content-Type` overriding the default, and the
  `MAX_RESPONSE_HEADERS` cap being enforced rather than overflowed.

## Socket Mocking Strategy (`test_connection.c`)
- Sockets are created in pairs via `socketpair(AF_UNIX, SOCK_STREAM, 0, fds)`.
- Client simulation writes directly into `fds[1]`, and server engine reads via `handle_readable` on `fds[0]`.
- Both ends are placed into non-blocking mode with `set_nonblocking` to ensure test assertions never block on incomplete reads.

## Memory Lifecycle Rules
- Every dynamically allocated `Connection` in test fixtures must be released via `connection_close(app, conn)` or direct `free` once assertions complete.
- Any AST produced by `json_parse` must be cleaned up via `json_free(root)`.
- Response buffers allocated by `res_send`/`res_json` (`conn->out_buf`) must be released to avoid leaks across test iterations.
