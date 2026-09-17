# tests/ — test harness & verification

## Architecture
Standalone unit and integration test suites compiled independently into `build/bin/test_*`. Tests run without third-party test frameworks using standard C `<assert.h>` assertions and return exit code 0 on complete pass:
- `test_json.c`: Pure data-structure tests covering JSON parsing, primitive tokens, nested arrays/objects, string escaping, and stringification round-trips.
- `test_middleware.c`: Middleware chain dispatching, synchronous execution ordering, termination by early response, 404/405 fallthrough (unmatched path vs. path matched under a different method), and error propagation via `chain_error`.
- `test_router.c`: Route registration limits (`MAX_ROUTES`), literal pattern matching, parameter tokenization (`:id`), `*` wildcard matching (mid-pattern and trailing), route param lookup (`req_get_param`), and `match_route_allowed_methods` (the method list behind a 405 response).
- `test_http_parser.c`: Pure buffer parsing of HTTP/1.1 request lines, query-string splitting (`parse_query_string`) and lookup (`req_get_query`), header parsing (`parse_headers`) and lookup (`req_get_header`), URL-decoding (`url_decode`), Content-Length bounds enforcement (including the 413-vs-400 sentinel split), a body beyond the old `BUF_SIZE`, and keep-alive header determination.
- `test_connection.c`: Non-blocking socket I/O, `handle_readable` buffer progression, keep-alive connection reuse, partial buffer reads, header buffer overflow cutoff (431 Request Header Fields Too Large / 400 Bad Request), a body beyond `BUF_SIZE` growing `conn->in_buf` (and shrinking it back down once idle), and a `Content-Length` beyond `MAX_BODY_SIZE` (413 Payload Too Large) via POSIX `socketpair(2)` connected to an isolated event-loop instance without binding to physical network ports.
- `test_response.c`: `res_set_header` behavior - custom headers appear in the built
  response, same-name overwrite (case-insensitive), `Content-Length`/`Connection`
  rejected as reserved, a custom `Content-Type` overriding the default, and the
  `MAX_RESPONSE_HEADERS` cap being enforced rather than overflowed.
- `test_multipart.c`: `multipart/form-data` boundary extraction (quoted/unquoted,
  trailing parameters, non-multipart rejection), part splitting (fields, file
  parts, a binary payload with an embedded NUL byte), malformed/nameless parts
  being skipped, and the `MAX_MULTIPART_PARTS` truncation cap.
- `test_urlencoded.c`: `application/x-www-form-urlencoded` body splitting on
  `&`/`=`, `%XX`/`+` decoding, bare-key-gets-empty-value and skipped-empty-pair
  edge cases, `NULL`/empty bodies, and the `MAX_FORM_FIELDS` truncation cap.
- `test_static.c`: Traversal prevention (`..` rejection, symlink escape checks),
  custom MIME types, prefix stripping, and directory index fallback against a
  temporary filesystem sandbox.
- `test_event_loop.c`: Cross-platform event-loop lifecycle (`event_loop_init`,
  `event_loop_close`), read/write readiness polling via `socketpair(2)`, and
  idle/shutdown timer expiration verification.

## Socket Mocking Strategy (`test_connection.c`)
- Sockets are created in pairs via `socketpair(AF_UNIX, SOCK_STREAM, 0, fds)`.
- Client simulation writes directly into `fds[1]`, and server engine reads via `handle_readable` on `fds[0]`.
- Both ends are placed into non-blocking mode with `set_nonblocking` to ensure test assertions never block on incomplete reads.

## Memory Lifecycle Rules
- Every dynamically allocated `Connection` in test fixtures must be released via `connection_close(app, conn)` or direct `free` once assertions complete.
- Any AST produced by `json_parse` must be cleaned up via `json_free(root)`.
- Response buffers allocated by `res_send`/`res_json` (`conn->out_buf`) must be released to avoid leaks across test iterations.
