# lib/ — engine internals

## Architecture
Single-threaded, non-blocking event loop on kqueue (`connection.c: app_listen`). One
`App` owns the listening socket, the kqueue fd, a fixed route table (`MAX_ROUTES`), and
a `Connection *` slot per possible fd. Unlike the route table, this connections table
is *not* a fixed-size array: `App.connections` (`app_types.h`) is a heap-allocated
`Connection **`, indexed directly by fd value, starting at `INITIAL_CONNECTION_TABLE_CAP`
(1024 slots, allocated by `app_init`, `router.c`) and grown — `realloc`, doubling, newly
added slots zeroed — by the new `ensure_connection_capacity` (`connection.c`, `static`)
whenever `accept_connections` sees an fd that doesn't fit yet (`App.connections_cap`
tracks the current allocation). There is no fixed ceiling: the real bound is the
process's own `RLIMIT_NOFILE`, since `accept()` itself starts failing with `EMFILE`
once that's hit — a fixed-size table would only ever be too small (an artificial cap
below what the OS already allows) or wastefully large. `app_destroy` (`connection.h`)
is the documented match for `app_init`'s allocation — see "Memory lifecycle" below.

Data flow per request: `handle_readable` (I/O) accumulates bytes into
`conn->in_buf` → `request_is_complete` (pure, `http_parser.c`) checks the buffer
without mutating it → `parse_http_request` (pure) fills a `Request` → `match_route`
(pure, `router.c`) looks up a `Handler` (or returns `NULL`) → `dispatch`
(`middleware.c`) runs the middleware pipeline ending at that handler (or a 404/405,
see "Deny-by-default routing" below) →
the handler calls `res_send`/`res_json` (`response.c`), which only builds bytes into
`conn->out_buf` and never touches the socket → `flush_connection` (I/O) is what
actually writes. This split exists so the parsing/routing/dispatch/response-building
layer stays pure and unit-testable independent of sockets:
- `tests/test_json.c` tests parsing, AST representation, and stringification.
- `tests/test_middleware.c` tests pipeline ordering, short-circuiting, 404 fallthrough, and error handlers.
- `tests/test_router.c` tests segment-by-segment tokenization, `:param` extraction, bounded param limits, `*` wildcard matching, and route table resolution.
- `tests/test_http_parser.c` tests pure request line, query-string splitting/lookup, header, Content-Length boundary extraction, and keep-alive parsing.
- `tests/test_connection.c` tests non-blocking socket I/O, `handle_readable` state progression, keep-alive persistence, partial buffer reads, 400 Bad Request on malformed inputs, 431 on header overflow, `in_buf` growth/shrink for a body beyond `BUF_SIZE`, and 413 on a `Content-Length` beyond `MAX_BODY_SIZE`, via POSIX `socketpair(2)` with a dedicated `kqueue()` instance without opening live TCP ports.
- `tests/test_multipart.c` tests `Content-Type` boundary extraction (quoted/unquoted,
  trailing parameters, non-multipart rejection), part splitting (fields, file parts,
  a binary payload with an embedded NUL byte), malformed/nameless parts being
  skipped, and the `MAX_MULTIPART_PARTS` truncation cap.
- `tests/test_urlencoded.c` tests `application/x-www-form-urlencoded` body
  splitting/decoding, bare-key/empty-pair handling, `NULL`/empty bodies, and the
  `MAX_FORM_FIELDS` truncation cap.
- `tests/test_static.c` tests the pure `static_resolve_relative_path`/
  `static_mime_type` (traversal rejection, prefix stripping, MIME lookup), and
  `static_serve_file` end-to-end against a real `mkdtemp`-created directory tree
  (a served file, a missing file, a `..` attempt, a symlink escaping the mount
  root, and the directory→`index.html` fallback).

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
  routes or for 404s. Registering more than `MAX_ROUTE_MIDDLEWARES` for one route
  truncates the list (stderr warning) rather than overflowing the fixed array, same
  as `MAX_ROUTES`/`MAX_MIDDLEWARES` elsewhere.
- App-wide middleware can also be *prefix*-scoped: `app_use_prefix(app, "/api", mw)`
  stores the prefix alongside the function pointer in `App.middlewares`
  (`MiddlewareEntry { fn, prefix }`, `app_types.h`) instead of a bare `Middleware`
  array. `app_use(app, mw)` is just `app_use_prefix(app, "", mw)`. `chain_next`
  (`middleware.c`) checks `middleware_prefix_matches(entry->prefix, req->path)`
  for each app-wide slot as it walks `chain->index` forward: a prefix of `""` or
  `"/"` matches every path (unscoped, the pre-existing behavior); otherwise `path`
  must start with `prefix` *and* either end there or be followed by `/`, so `/api`
  matches `/api` and `/api/users` but not `/apiary`. A non-matching entry is
  skipped without being invoked (and therefore never calls `chain_next` itself) —
  `chain_next` loops internally past skipped entries rather than relying on each
  middleware to re-trigger the chain. `chain->index` still counts every app-wide
  slot (matched or skipped), so `route_index = chain->index - chain->count` for
  locating route-middleware position is unaffected. Skipped middleware also means
  the ordering guarantee is now "registration order, filtered to prefix-matching
  entries" rather than strictly every registered middleware running on every
  request — this applies to 404s too (an unmatched route still runs prefix-scoped
  middleware whose prefix matches `req->path`).
- Because `chain_next` is a plain synchronous call (no deferred/async dispatch
  anywhere in this engine), a middleware that calls `chain_next(chain)` and then does
  more work afterward (e.g. logging) runs that code *after* the entire rest of the
  pipeline — including the handler — has already finished building the response, so
  `res->status` is final by then. See `app/middlewares.c: mw_logger`.
- Route `Handler`s themselves are not given a `MiddlewareChain *` and so cannot call
  `chain_next`/`chain_error` — they remain the terminal node of the pipeline exactly
  as before; only middleware (app-wide or per-route) participates in chaining.

## Sub-router mounting
`Router` (`app_types.h`) is a standalone route table — its own
`Route routes[MAX_ROUTES]` + `route_count` and its own router-level
`Middleware middlewares[MAX_MIDDLEWARES]` + `middleware_count` — built up via
`router_get`/`router_post`/`router_put`/`router_patch`/`router_delete` (plus
their `_mw` variants) and `router_use` (`router.c`), the exact `Router`
analogues of `app_get`/`app_post`/`app_put`/`app_patch`/`app_delete`/`app_use`.
A `Router` has no effect on dispatch by itself; it only
takes effect once mounted into an `App` via `app_mount(app, prefix, router)`
(the C analogue of Express's `app.use('/api', router)`):
- **Routes are flattened, not nested.** `app_mount` copies each of the router's
  `Route`s into `app->routes` (via `app_add_route_mw`, same `MAX_ROUTES` cap and
  truncation-with-warning as any other route registration) with `prefix`
  prepended to the route's own path (`build_mounted_path`, `router.c`). A route
  registered at the router's own root (`router_get(router, "/", h)`) mounts at
  `prefix` itself rather than `prefix + "/"` — so it matches `GET /api`, not
  `GET /api/`. There is no `Route` back-reference to the `Router` it came from,
  no runtime indirection through the `Router` struct at all — after `app_mount`
  returns, the `Router` can go out of scope (e.g. a stack-local in `main`)
  without dangling anything live in `app`.
- **Router-level middleware becomes prefix-scoped app-wide middleware**, not a
  separate pipeline stage: `app_mount` calls `app_use_prefix(app, prefix, mw)`
  for each of the router's `router_use`-registered middleware. This reuses the
  prefix-matching machinery above verbatim — a router's middleware runs ahead of
  route dispatch for *any* request under `prefix`, including one that 404s
  within the router's own route set, exactly like directly-registered
  prefix-scoped middleware would.
- **`prefix` normalization** (`app_mount`, `router.c`): `""` or `"/"` mounts
  unscoped — router routes keep their own path unmodified and router middleware
  runs on every request, same as `app_use_prefix(app, "", mw)`. A trailing slash
  (`"/api/"`) is stripped before concatenation so a router route doesn't end up
  double-slashed (`"/api//users"`).
- **Registration order still governs middleware order.** Because mounting just
  calls `app_use_prefix` at the point `app_mount` runs, a router's middleware is
  interleaved into `app->middlewares` wherever that call happens relative to
  other `app_use`/`app_use_prefix`/`app_mount` calls — mount your routers in the
  order you want their middleware to run relative to each other and to any
  directly-registered app-wide middleware.
- This flattening approach means there's still no *nested* mounting (a `Router`
  containing another `Router`) and no runtime concept of "which router a route
  came from" — `match_route`/`dispatch` are completely unchanged, they just see
  a bigger flat `app->routes` table. See `tests/test_router.c`
  (`test_app_mount_*`) for prefixing, root-route, middleware-scoping, and
  `MAX_ROUTES` truncation coverage.

## Deny-by-default routing
`match_route` returns `NULL` on no match; there is no implicit fallback handler, so
an unregistered path or method is always rejected rather than silently served (a
route can opt into acting as an explicit catch-all via a trailing `*` pattern —
see "Route wildcards" below — but that's an app author's deliberate choice, not a
built-in default). `match_route` requires an exact `Route.method` string match (`router.c`), so
registering `GET /users/:id` does not make `PUT /users/:id` match it — `match_route`
itself returns `NULL` either way, regardless of *why* nothing matched.

`dispatch` (`middleware.c`) is what turns a `NULL` route into an actual response,
and it tells the two `NULL` cases apart before building the `MiddlewareChain`: it
calls `match_route_allowed_methods` (`router.c`) — which walks every registered
route checking `match_path` only (method-agnostic) and collects a deduplicated,
registration-ordered, comma-separated method list — and stores the result on the
chain (`method_not_allowed` + `allowed_methods`). `chain_next`'s final fallback
(after every app-wide and route middleware has run, same as the plain-404 path)
then sends 404 when nothing matches the path at all, or 405 + an `Allow:
<methods>` response header when the path matches at least one route under a
different method. `match_route_allowed_methods` never mutates the caller's
`Request` (it runs `match_path` against a local scratch copy), so it's safe to
call speculatively even though `match_route` already tried and failed. See
`tests/test_middleware.c` (`test_dispatch_returns_405_*`,
`test_dispatch_returns_404_*`) and `tests/test_router.c`
(`test_match_route_allowed_methods_*`).

Convenience wrappers exist for `GET`/`POST`/`PUT`/`PATCH`/`DELETE`/`HEAD`/`OPTIONS`
(`app_get`/`app_post`/`app_put`/`app_patch`/`app_delete`/`app_head`/`app_options`,
each with an `_mw` variant, plus the `router_*` equivalents) — see "HEAD/OPTIONS
auto-handling" below for what happens when a path has no explicit `HEAD`/`OPTIONS`
route registered at all.

## HEAD/OPTIONS auto-handling
An explicit `app_head`/`app_options`/`router_head`/`router_options` route (plus
their `_mw` variants, `router.c/h`) always wins when registered — both are thin
wrappers around `app_add_route(_mw)`/`router_add_route(_mw)` exactly like
`app_put`/`app_patch`/`app_delete`, and `match_route` (below) always tries an
exact method match before either fallback described here kicks in. Without an
explicit registration, two auto-behaviors mirror Express:
- **Auto-HEAD-from-GET** lives in `match_route` (`router.c`): once the loop over
  every registered route finds no exact `HEAD` match, a second pass looks for
  the first route matching the same path under `GET` and returns that instead.
  The `GET` handler then runs completely normally — same middleware, same
  `res_send`/`res_json` call building a real body — nothing downstream of
  `match_route` knows this was a fallback.
- **Body suppression** is what actually makes a `HEAD` response spec-correct
  (RFC 7231 4.3.2: a `HEAD` response must carry the same headers, including
  `Content-Length`, a `GET` would have, but never a body — for *any* status,
  not just when the auto-fallback above is involved). `Response.is_head_request`
  (`app_types.h`) is set by `handle_readable` (`connection.c`) from
  `req->method` right after a successful parse, *before* `dispatch()` runs —
  so it's already in place whether the pipeline ends at a matched handler or
  at chain_next's 404/405 fallback. `send_with_content_type` (`response.c`)
  still computes `Content-Length` from the handler's full body string either
  way, it just skips copying those bytes into `conn->out_buf` when the flag is
  set — the one-`malloc`-sized-to-header+body pattern becomes
  header-only. Every other `Response` local in `connection.c` (the
  431/500/408 rejection paths, which run ahead of or without a full parse)
  explicitly leaves this at `0` — those responses have no real body worth
  suppressing anyway.
- **Auto-OPTIONS** lives in `chain_next`'s final fallback (`middleware.c`),
  the same spot the 404-vs-405 decision (below) is made: when `route` is
  `NULL` (no exact match, no `HEAD`→`GET` fallback applicable) and the
  request method is `OPTIONS`, and `match_route_allowed_methods` (already
  computed by `dispatch()` for the 405 case) shows the path matches at least
  one route under some other method, `chain_next` sends `200` + an `Allow`
  header instead of `405` — an `OPTIONS` request to a path matching nothing
  at all still falls through to the ordinary `404`. The `Allow` list reuses
  `match_route_allowed_methods`' registration-ordered, deduplicated string
  and augments it with two implicit entries via `method_list_contains` (a
  small token-exact substring check, `middleware.c`): `HEAD` is added
  whenever `GET` is present and `HEAD` isn't already explicit (since `HEAD`
  works there via the fallback above even with no route of its own), and
  `OPTIONS` is always added, since reaching this branch means it's
  implicitly supported for that path.
See `tests/test_router.c` (`test_app_head_options_register_correct_methods`,
`test_router_head_options_register_correct_methods`,
`test_match_route_head_falls_back_to_get`,
`test_match_route_explicit_head_wins_over_get_fallback`),
`tests/test_middleware.c` (`test_dispatch_auto_options_*`,
`test_dispatch_options_still_404s_unknown_path`,
`test_dispatch_explicit_options_route_wins_over_auto`),
`tests/test_response.c` (`test_head_response_omits_body_but_keeps_content_length`),
and `tests/test_connection.c` (`test_handle_readable_head_request_omits_body`,
`test_handle_readable_auto_options_response`).

## Route wildcards
`match_path` (`router.c`) tokenizes both the route pattern and the request path on
`/` and compares them segment by segment, same as it always has for literal
segments and `:name` params. A pattern segment that is exactly `*` is a wildcard,
handled differently depending on where it falls:
- **Mid-pattern** (any `*` segment with more pattern segments after it): matches
  exactly one path segment, like `:name` but without capturing anything into
  `req->param_*` — there's no name to capture under. Matching then resumes
  normally against the rest of the pattern, so a pattern with a literal segment
  after the `*` still requires that literal to match at the same position.
- **Trailing** (a `*` segment with nothing after it in the pattern): matches that
  segment *and* everything remaining in the path, so the loop returns a match
  immediately without consuming the rest of `path`'s tokens one by one. This is
  the "catch-all" shape (`app_get(&app, "/files/*", handler)` matches
  `/files/report.pdf` and `/files/2024/q1/report.pdf` alike) — but it still
  requires at least one path segment after the literal prefix, so it does *not*
  match the prefix alone (`/files/*` does not match plain `/files`, since there's
  no segment there for `*` to match against).
`*` and `:name` segments can be mixed on the same pattern (`/users/:id/*`).
Wildcards participate in ordinary `match_route`/`dispatch` resolution exactly like
any other route — including the 405-vs-404 distinction above, since
`match_route_allowed_methods` also runs `match_path` under the hood. See
`tests/test_router.c` (`test_match_path_wildcards`) and `app/main.c`
(`GET /files/*` → `handler_files`).

## Static file serving
`app_serve_static(app, prefix, root_dir)` (`router.c`) registers a static-file mount
as a `Route` rather than a `Handler` call — it builds a trailing-wildcard pattern
(`"<prefix>/*"`, reusing the machinery above) via the same `fill_route` every other
registration function uses, then overwrites that `Route`'s `static_root` field
(`app_types.h`, empty `""` for every ordinary route — `fill_route` explicitly clears
it, since `Route` slots aren't zero-initialized on their own and stale stack garbage
could otherwise look like a static mount) with `root_dir` canonicalized via
`realpath()` **at registration time** — if `root_dir` doesn't exist, this logs a
warning and registers nothing at all (deny-by-default, same as every other hard
failure in this engine's registration functions). `route->handler` stays `NULL`: a
plain `Handler(const Request*, Response*)` has no way to receive the mount's root
directory, so a static route is never dispatched through one. Instead,
`MiddlewareChain` now also carries the matched `route` itself (not just the
`Handler` derived from it), and `chain_next`'s final fallback (`middleware.c`)
checks `route->static_root[0] != '\0'` *before* the `final_handler != NULL` check
and calls `static_serve_file` (`static.c/h`) directly instead. App-level only —
there's no `router_serve_static`/`app_mount` equivalent yet.

`static_serve_file(route, req, res)` is the one impure function in `static.c`;
everything else is pure and unit-tested without touching a filesystem
(`tests/test_static.c`):
- **`static_resolve_relative_path(mount_pattern, req_path, out, out_size)`** strips
  the mount's literal prefix off `req_path` (already URL-decoded by
  `parse_http_request` before routing ever runs — see "URL decoding" above) and
  walks the remainder segment by segment, rejecting (`-1`) any segment that is
  exactly `".."` — the primary traversal defense, and why this function is pure:
  it never needs to touch the filesystem to catch a `..`-shaped request. A `req_path`
  that doesn't start with the mount's prefix, or resolves to nothing after stripping
  (a request for the mount root itself — which a trailing wildcard route never
  matches anyway, see "Route wildcards" above), is also rejected.
- **`static_mime_type(path)`** is a small, demo-sized extension → MIME-type lookup
  table, defaulting to `application/octet-stream` for anything not in it.
- **`static_serve_file`** resolves `route->static_root + "/" + subpath` via
  `realpath()` and — critically — re-checks that the *resolved* path still starts
  with `route->static_root` at a `/` boundary. This is defense-in-depth on top of
  the textual `..` check above: a symlink living inside the mount's root can point
  outside it without any `..` ever appearing in the request path, and `realpath()`
  is what actually follows that symlink. A directory request retries once against
  `<resolved>/index.html` through the same containment check (Express's default
  directory-index behavior) — there is no directory listing, ever. Status mapping:
  `403` for a rejected/escaping path, `404` for anything that doesn't resolve to a
  regular file even after the `index.html` retry, `500` if the file exceeds the new
  `MAX_STATIC_FILE_SIZE` cap (`app_types.h`, 50 MiB — there's no streaming response
  path yet, see `pending.txt` item #2) or a read fails partway through, `200`
  otherwise. The whole file is read into one `malloc`'d buffer and sent via the new
  `res_send_bytes` (below), freed immediately after.

**`res_send_bytes(res, content_type, data, len)`** (`response.c/h`) is a new sibling
to `res_send`/`res_json` for a byte buffer of known length rather than a
NUL-terminated C string — a served file (an image, a font, ...) can contain
embedded NUL bytes, which `strlen`-based `res_send`/`res_json` would silently
truncate (the same class of bug already fixed for `req->body`, see "Body
buffering" below). `send_with_content_type` (`response.c`) now takes an explicit
`body_len` parameter instead of computing `strlen(body)` itself; `res_send`/
`res_json` are unchanged in behavior, they just pass `strlen(body)` through
themselves.

See `tests/test_static.c` (pure coverage for `static_resolve_relative_path`/
`static_mime_type`, plus an I/O suite against a real `mkdtemp`-created directory
tree — including a symlink escaping the mount root — driven directly against
`static_serve_file`) and `app/main.c`/`app/CLAUDE.md` (`GET /static/*` →
`app/public/`) for a real usage example.

## Query-string parsing
`parse_http_request` (`http_parser.c`) still stores the raw query string as-is in
`req->query` (everything after `?` in the request line, unparsed), but now also
calls `parse_query_string(req->query, req)` — a pure function taking the query
string as a `const char *` buffer, per this project's parsing-function
convention — to split it into `req->query_names`/`req->query_values` (fixed
`MAX_QUERY_PARAMS`-sized arrays + `query_count`, the same shape as
`param_names`/`param_values`/`param_count` for route params). Splitting rule: `&`
separates pairs, `=` separates a pair's key from its value; a pair with no `=`
(e.g. a bare `flag`) gets an empty-string value rather than being dropped.
Consecutive `&`s don't produce empty pairs (`strtok_r` skips repeated
delimiters, same as `match_path` does for repeated `/`). Pairs beyond
`MAX_QUERY_PARAMS` are dropped rather than overflowing the fixed arrays, same
pattern as `MAX_ROUTES`/`MAX_PARAMS` elsewhere. `req_get_query(req, name)`
(`http_parser.c`) looks the parsed array up linearly and returns the *first*
matching value, mirroring `req_get_param`. Both the key and value of every
pair are URL-decoded (see "URL decoding" below) before being stored, so
`req_get_query`/`req->query_names`/`req->query_values` all see decoded bytes
even though `req->query` itself stays raw. See `tests/test_http_parser.c`
(`test_parse_query_string`) and `app/main.c` (`GET /search?q=...` →
`handler_search`, via `req_get_query(req, "q")`).

## URL decoding
`url_decode(src, dst, dst_size, decode_plus)` (`http_parser.c`) is a pure
percent-decoder: `%XX` hex escapes always decode to the literal byte, and
`+` decodes to a space only when `decode_plus` is set — the
`application/x-www-form-urlencoded` convention for query-string keys/values,
not path segments. A `%` not followed by two hex digits is copied through
literally rather than decoded or dropped, so a caller never has to guard
against a malformed escape shrinking or corrupting unrelated bytes. Decoded
output is never longer than the input, so every call site decodes in place
(`dst == src`) rather than needing a second buffer.
- `parse_http_request` runs `url_decode(req->path, req->path, ..., 0)` right
  after splitting the query string off the request-line path, and before
  `parse_query_string`/routing ever run — so `req->path`, `match_path`'s
  segment comparisons, and `req_get_param` (which captures straight from
  `req->path`) all operate on decoded bytes. `match_path`/`req_get_param`
  themselves do no decoding of their own; decoding only ever happens once,
  at parse time.
- `parse_query_string` runs `url_decode(..., 1)` over both the key and value
  of each pair it splits out, so `req_get_query` returns decoded values (see
  "Query-string parsing" above). `req->query` (the raw string the pairs were
  split from) is left untouched.
- **Known gap:** decoding `req->path` happens before `match_path` tokenizes
  it on `/`, so a client-encoded `%2F` decodes to a literal `/` ahead of
  segment splitting and can merge two path segments the client intended to
  keep separate into one. There's no special-casing of `%2F` to avoid this
  (see `pending.txt`).
See `tests/test_http_parser.c` (`test_url_decode`, the encoded-path case in
`test_parse_http_request`).

## Header parsing
`req->headers` (`app_types.h`) stays the raw, unparsed header block it always
was (everything between the request line and the blank line terminator), but
`parse_http_request` now also calls the new `parse_headers(req->headers, req)`
(`http_parser.c`) — a pure function, per this project's parsing-function
convention — right after `req->headers` is filled, before `extract_content_length`
runs (which still scans the raw blob itself, unchanged). `parse_headers` splits
the block on `"\r\n"` then each line's first `':'` into
`req->header_names`/`req->header_values` (fixed `MAX_HEADERS`-sized arrays +
`header_count`, the same bounded-array-plus-count shape as
`param_names`/`query_names` elsewhere — extra headers past the cap are dropped
rather than overflowing). Leading spaces after the `':'` are trimmed; a line
with no `':'` is skipped rather than stored. `req_get_header(req, name)`
(`http_parser.c`) looks the array up with `strcasecmp` — header field names are
case-insensitive per RFC 7230, unlike `req_get_query`/`req_get_param`'s
case-sensitive `strcmp` — and returns the first matching value. Header values
are **not** URL-decoded (headers aren't a URL component, unlike the path/query
handled above). `app/middlewares.c: mw_authenticate` reads `Authorization` via
`req_get_header` rather than its own ad-hoc `strstr(req->headers, ...)` scan —
this also fixed a latent case-sensitivity bug, since `strstr` never matched a
lowercase `authorization:` header the way the new case-insensitive lookup does.
See `tests/test_http_parser.c` (`test_parse_headers`) and `app/CLAUDE.md`
("Content-Type-gated body validation").

**Zero-header requests are a real edge case, not a hypothetical one.**
`parse_http_request` locates the header block via `header_start` (the first
`"\r\n"` in `raw`, i.e. right after the request line, `+2`) and `header_end`
(the first `"\r\n\r\n"`, i.e. the blank-line terminator). When at least one
header line is present these never cross — `header_start` lands before
`header_end` naturally. But a request with *no* header lines at all (request
line's own `\r\n` immediately followed by the blank line's `\r\n`, e.g.
`"GET / HTTP/1.1\r\n\r\n"`) makes the *first* `"\r\n"` in `raw` coincide with
the start of that `"\r\n\r\n"` itself, so `header_start` (`+2`) lands two
bytes *past* `header_end`. Previously this pointer subtraction wrapped to a
huge `size_t`, which the existing `>= sizeof(req->headers)` clamp then capped
to `8191` — turning an ordinary, spec-legal header-less request into an
out-of-bounds heap read of up to 8191 bytes via the `memcpy` into
`req->headers` (found via ASan; reproduces on the live server with e.g.
`curl -H "Host:" ...` to suppress curl's default `Host` header). `parse_http_request`
now clamps `header_start` down to `header_end` whenever the former would
exceed the latter, which is a no-op for every request with real headers and
makes the zero-header case resolve to an empty header block (`header_len ==
0`) instead of reading garbage. See `tests/test_http_parser.c`
(the zero-header case in `test_parse_http_request`).

## Cookies
Request-side and response-side cookie handling are two independent pieces,
each following an existing convention rather than introducing a new one:
- **Reading** (`lib/http_parser.c/h`): `parse_cookies(cookie_header, req)` is a
  pure function, called from `parse_http_request` as
  `parse_cookies(req_get_header(req, "Cookie"), req)` right after `parse_headers`
  fills `req->header_*` (so the `Cookie` header, if any, is already available to
  look up). It splits the header value on `';'` then the first `'='` into
  `req->cookie_names`/`req->cookie_values` (fixed `MAX_COOKIES`-sized arrays +
  `cookie_count`, the same bounded-array-plus-count shape as
  `header_names`/`query_names` elsewhere — extra pairs past the cap are dropped
  rather than overflowing). Leading spaces after `';'` are trimmed; a pair with
  no `'='` is skipped rather than stored, same as `parse_headers` skipping a
  line with no `':'`. `cookie_header` may be `NULL` (no `Cookie` header sent at
  all), treated the same as an empty string — `req.cookie_count` just stays `0`,
  this is not a parse error. `req_get_cookie(req, name)` looks the array up with
  `strcmp` — cookie names are case-*sensitive* per RFC 6265, unlike
  `req_get_header`'s case-insensitive `strcasecmp` — and returns the first
  match. Values are **not** URL-decoded, same reasoning as header values (a
  `Cookie` header isn't a URL component either). Because `req->header_values`
  entries (and so the raw `Cookie` header value `parse_cookies` splits) are
  capped at 256 bytes each, a request sending an unusually large number of
  cookies can already be truncated before `parse_cookies` ever sees the rest —
  an existing per-header-value limit, not something specific to cookies.
- **Writing** (`lib/response.c/h`): `res_set_cookie(res, name, value, options)`
  formats one RFC 6265 `Set-Cookie` header value (`name=value` plus whichever
  attributes `options` — `CookieOptions`, `app_types.h` — requests: `Path`
  defaulting to `"/"` when `options->path` is `NULL`, optional `Domain`,
  optional `Max-Age` when `options->max_age >= 0` (a valid explicit `0` expires
  a cookie immediately, so "omit this attribute" needs its own sentinel,
  `-1`), optional `HttpOnly`/`Secure` flags, optional `SameSite`) directly into
  a slot in `Response.set_cookies` (`app_types.h`) — a **separate** fixed array
  from `Response.headers`, not a `res_set_header` call, because a response can
  carry more than one `Set-Cookie` line at once and `res_set_header` overwrites
  same-name entries. `options` may be `NULL` for an all-default session
  cookie. Bounded by `MAX_RESPONSE_COOKIES`; extra calls past the cap, or a
  formatted cookie longer than `MAX_SET_COOKIE_LEN`, are dropped with a stderr
  warning rather than overflowing/truncating into a malformed line — same
  convention as `res_set_header`/`MAX_RESPONSE_HEADERS`. `send_with_content_type`
  emits one `"Set-Cookie: ...\r\n"` line per stored entry, in its own loop
  alongside (not merged into) the `headers[]` loop. `res_clear_cookie(res, name,
  path)` is `res_set_cookie` with `Max-Age=0` and an empty value — `path` must
  match whatever `Path` the cookie was originally set with (`NULL` defaults to
  `"/"`, same as `res_set_cookie`'s own default) for a browser to actually
  delete it, mirroring Express's `res.clearCookie(name)`.
- See `tests/test_http_parser.c` (`test_parse_cookies`, and the `Cookie`-header
  case in `test_parse_http_request`), `tests/test_response.c`
  (`test_set_cookie_defaults`, `test_set_cookie_with_options`,
  `test_multiple_cookies_each_get_own_line`,
  `test_clear_cookie_expires_immediately`,
  `test_max_response_cookies_enforced`), and `app/handlers.c`
  (`handler_login`/`handler_whoami`/`handler_logout`, `app/CLAUDE.md`) for a
  real usage example.

## Request size limits
Headers and body are now bounded independently, at very different sizes - see
"Body buffering" below for the body side. Headers alone are still hard-capped at
`BUF_SIZE` (8192 bytes), `conn->in_buf`'s starting capacity, which body growth never
touches (growth only happens once a complete header block has already been seen -
see below):
- `extract_content_length` (`http_parser.c`) returns a negative sentinel for a
  negative (`-1`) or oversized (`> MAX_BODY_SIZE`, `-2`) `Content-Length`;
  `parse_http_request` turns either into a parse failure (`-1`) rather than trusting
  or truncating it, and `connection.c` re-checks which sentinel it was to answer 400
  vs 413 (see "Body buffering").
- `handle_readable` (`connection.c`) detects when `in_buf` fills up
  (`in_len >= in_cap - 1`) with no `"\r\n\r\n"` header terminator ever having
  appeared - i.e. the headers themselves don't fit in `BUF_SIZE` - and responds 431 +
  closes instead of leaving the connection open indefinitely (the Slowloris-shaped
  gap noted in `../pending.txt`). This is unaffected by body growth: growth only
  ever triggers once headers are already complete.

The buffer-size cutoffs above only reject a request once it's known to be too big;
they do nothing about one that's simply too slow to arrive. That gap is closed by
the idle/read timeout below.

## Idle/read timeout
`Connection.last_activity` (`app_types.h`) is a `time_t` set at `connection_create`
(so a connection that never sends a single byte is still bounded) and advanced by
`handle_readable` on every successful `recv()`. A dedicated `EVFILT_TIMER`
registered once in `app_listen` (ident `1` — timer idents live in their own
kqueue namespace, so this never collides with a real connection fd) fires every
`IDLE_SWEEP_INTERVAL_MS` (1s) and, on each fire, `app_listen`'s event loop calls
`close_idle_connections(app)` (`connection.c`) instead of routing the event through
the normal read/write dispatch. `close_idle_connections` walks `app->connections`
(same direct fd-indexed array `accept_connections`/`connection_close` use) and, for
every connection whose `now - last_activity >= IDLE_TIMEOUT_SECONDS` (60s):
- **A write still in flight is left alone even if stale** (`conn->out_buf != NULL`
  is skipped outright) — that's a slow reader on the *response*, a different
  problem than this timeout targets (a slow *sender* of the request), and tearing
  it down here would kill a healthy connection mid-flush. `flush_connection`'s own
  `EVFILT_WRITE` retry loop is what eventually resolves that connection either way.
- **A partial request already buffered** (`conn->in_len > 0` — headers or body
  mid-arrival when the client went quiet) gets a `408 Request Timeout` response
  before closing, built and sent the same way the 431/500 rejections in
  `handle_readable` are (`res_send`/`flush_connection`, not a raw `close()`).
- **An idle keep-alive connection with nothing buffered** (`conn->in_len == 0` —
  waiting on a next request that never came) is torn down via `connection_close`
  directly, with no response to send.
`IDLE_TIMEOUT_SECONDS`/`IDLE_SWEEP_INTERVAL_MS` are compile-time constants in
`app_types.h`, not `ServerConfig` fields — consistent with every other hard limit
in this engine (`BUF_SIZE`, `MAX_BODY_SIZE`, ...). See `tests/test_connection.c`
(`test_close_idle_connections_*`), which drives this deterministically by writing
a synthetic past `last_activity` rather than sleeping in real time.

## Body buffering
A request body can be far larger than `BUF_SIZE` without being rejected, as long as
it's within `MAX_BODY_SIZE` (10 MiB, `app_types.h`) - this is what closes the
`../pending.txt` gap "Whole request (headers+body) lives in one fixed BUF_SIZE=8192
buffer - no streaming". This is *not* true incremental streaming to the handler
(`Handler` is unchanged - still synchronous, still gets a fully-buffered `req->body`)
- it's closer to how `express.json()`/`express.urlencoded()` behave in a real
Express app: the whole body is buffered up to a configurable size limit, then handed
to the route handler synchronously as one already-materialized value. See
`improvements.md` for what *true* streaming (the body delivered to a handler
incrementally, off a `Transfer-Encoding: chunked` request) would additionally
require.
- **`conn->in_buf` (`Connection`, `app_types.h`) is heap-allocated**
  (`connection_create`), not a fixed array - it starts at `BUF_SIZE` and
  `handle_readable` (`connection.c`) grows it via `realloc` only when both (a) the
  current capacity is exhausted (`in_len >= in_cap - 1`) and (b) a complete header
  block has already been seen (so growth can never mask an oversized-*header*
  attack - that's still always 431, see "Request size limits"). When it grows, it
  grows once, straight to the *exact* capacity the declared `Content-Length` needs
  (`header_len + content_length + 1`), not incrementally - the declared length is
  already known by that point, so there's no need to double-and-retry the way a
  general-purpose growable buffer normally would. A failed `realloc` (genuine
  server-side OOM - a client-declared size that's merely too large never reaches
  this far, see below) responds 500 and closes the connection; the original block is
  left untouched by a failed `realloc`, so nothing is corrupted, there's just no
  more room to grow into.
- **A `Content-Length` beyond `MAX_BODY_SIZE` never reaches the growth logic at
  all** - `request_is_complete` (`http_parser.c`, called after every `recv()`
  already, unchanged by this) treats `extract_content_length`'s `-2` sentinel the
  same as `-1` (any negative value means "stop buffering, let the caller reject
  it"), so the request is dispatched as soon as headers complete, and
  `handle_readable`'s failure branch re-checks `extract_content_length` itself to
  tell `-1` (malformed → 400) apart from `-2` (too large → 413) — see
  `tests/test_connection.c` (`test_handle_readable_body_too_large_413`).
- **`Request.body` (`app_types.h`) is `char *`, not a fixed array** - unlike every
  other `Request` field, it's `malloc`'d by `parse_http_request` (`http_parser.c`),
  sized to exactly `content_length + 1` bytes and `memcpy`'d out of `raw` (never
  aliased/pointer-shared with `conn->in_buf`, and never more than `content_length`
  bytes even if `raw` holds trailing bytes past the body - e.g. a pipelined next
  request already sitting in the same buffer). `raw` itself is never mutated -
  `parse_http_request` stays a pure function. The caller owns the result and must
  `free()` it - `handle_readable` does so right after `dispatch` returns, on every
  path (a failed parse still leaves `req->body` at `NULL` via `parse_http_request`'s
  initial `memset`, so `free(NULL)` there is always safe). See
  `tests/test_http_parser.c` (the large-body case in `test_parse_http_request`) and
  `tests/test_connection.c` (`test_handle_readable_large_body_grows_buffer`).
- **`parse_http_request` takes an explicit `raw_len`, not just `raw`** - the request
  line and headers are still located via NUL-terminated-string functions
  (`sscanf`/`strstr`, legitimate since neither can contain embedded NULs), but how
  many body bytes are available past `header_end + 4` is computed from `raw_len`
  (`connection.c` passes `conn->in_len`), not `strlen(body_start)`. A body can
  contain arbitrary bytes - a binary file inside a `multipart/form-data` part (see
  "multipart/form-data parsing" below) - and `strlen` would stop at the first
  embedded NUL, silently truncating a request that arrived in full. This was a
  latent bug even before multipart support existed; nothing previously sent a body
  with a real NUL byte in it, so it never surfaced. See the embedded-NUL case in
  `tests/test_http_parser.c: test_parse_http_request`.
- **`in_buf` shrinks back down once idle.** `flush_connection`'s keep-alive reset
  (`connection.c`) `realloc`s `in_buf` back to `BUF_SIZE` whenever `in_cap` grew past
  it, so one large request doesn't permanently inflate a long-lived keep-alive
  connection's memory footprint. A failed shrink isn't fatal (`realloc` leaves the
  larger block untouched) - the connection just keeps using the bigger buffer.

## Chunked Transfer-Encoding
A `Transfer-Encoding: chunked` request body is an alternative to a declared
`Content-Length` (previous section) - the body's size isn't known upfront, it's
split into `"<hex-size>[;ext]\r\n<data>\r\n"` chunks terminated by a `0`-size
last-chunk, an optional trailer-part, and a final CRLF (RFC 7230 4.1). Three
pure functions in `lib/http_parser.c/h` implement this, mirroring the
Content-Length path's shape at every call site rather than introducing a
separate code path:
- **`request_has_chunked_encoding(header_block)`** - true when a
  `Transfer-Encoding` header's value contains `"chunked"` (case-insensitive),
  scoped to just that header's own line via a bounded stack copy so a
  `"chunked"` appearing elsewhere (another header, or body bytes when called
  on the whole raw connection buffer) can't false-match. Called both on the
  raw buffer, before headers are even sliced out (`request_is_complete`), and
  on the already-isolated `req->headers` block (`parse_http_request`) - the
  same dual-use pattern `extract_content_length` already follows.
- **`chunked_body_scan(body_start, available, max_decoded_len,
  decoded_len_out)`** - walks the chunk framing without allocating or copying
  any data, returning `1` (fully received), `0` (need more bytes), `-1`
  (malformed framing - non-hex size, a chunk's data not followed by its own
  CRLF, or a chunk-size line longer than `MAX_CHUNK_SIZE_LINE_LEN` can
  legitimately be), or `-2` (the running decoded size, tracked in
  `*decoded_len_out` across chunks, already exceeds `max_decoded_len`). The
  `-2` check happens against each chunk's *declared* size as soon as its
  size-line is parsed, before that chunk's data has necessarily arrived -
  the same early-rejection shape `extract_content_length` gives a
  Content-Length beyond `MAX_BODY_SIZE`, just applied cumulatively across
  chunks instead of to one header value. `request_is_complete` calls this
  with `max_decoded_len = MAX_BODY_SIZE` and stops buffering (returns `1`) on
  any non-zero result, positive or negative - `parse_http_request` re-derives
  which one happened, same pattern the plain Content-Length branch already
  uses its own negative sentinels for.
- **`chunked_body_decode(body_start, available, out)`** - decodes a body
  `chunked_body_scan` has already confirmed complete (`1`) for this exact
  input into `out` (a caller-owned buffer sized to that same call's
  `decoded_len_out`), returning the number of bytes written. Does not
  re-validate framing - same trust-the-precondition contract
  `parse_multipart_body` has toward a prior successful
  `multipart_parse_boundary` call. Decoded data can contain arbitrary bytes,
  including embedded NULs (a chunked binary upload) - callers must use the
  return value, never `strlen(out)`, same convention as multipart/urlencoded
  body parsing.

`parse_http_request` branches on `request_has_chunked_encoding(req->headers)`
right after headers are parsed: a `Content-Length` header present
*alongside* `Transfer-Encoding: chunked` is rejected outright (`-1` -> 400) -
RFC 7230 3.3.3 calls this combination ambiguous about where the body actually
ends, a request-smuggling shape rather than a client mistake, so neither
header is preferred over the other. Otherwise `chunked_body_scan` re-confirms
completeness (`request_is_complete` already required this before
`parse_http_request` was ever called) and, on success, `chunked_body_decode`
fills `req->body` - `req->content_length` is set to the *decoded* size
(matching what a handler reading it would expect from the Content-Length
path), not the raw wire size. A `chunked_body_scan` result of `-2` sets
`req->content_length = -2` before failing - the same sentinel
`extract_content_length` returns for an oversized declared Content-Length -
so `connection.c`'s `req.content_length == -2 -> 413` check covers both
oversized cases without connection.c needing to know which framing was used.

`connection.c: handle_readable`'s buffer-growth branch (reached when `in_buf`
fills up with headers already complete) also branches on
`request_has_chunked_encoding`: a chunked body has no single declared size to
`realloc` straight to the way a Content-Length body does, so it grows
geometrically (doubling) instead, capped at `header_len + MAX_BODY_SIZE` - the
same ceiling a Content-Length body gets, but applied here to the *raw* wire
size rather than the decoded size `chunked_body_scan` already bounds
independently. This raw-side cap matters on its own: without it, a client
could inflate server memory well past `MAX_BODY_SIZE` by sending the same
decoded byte count as a pile of pathologically tiny chunks (each
`"1\r\nX\r\n"` chunk costs 6 raw bytes per 1 decoded byte) before the
decoded-size check would otherwise trigger. Reaching this branch at all means
`chunked_body_scan` already returned exactly `0` on the current buffer (`1`/
`-1`/`-2` all end the request via the dispatch branch above instead, on an
earlier `recv()`), so growth here never needs to re-examine *why* - just
whether there's still room to grow under the cap. Hitting the cap while still
incomplete rejects with 413 directly, rather than falling through to the
generic realloc-failure-shaped 500 the Content-Length path's fallback uses.

**Known trade-off:** the raw-wire-size cap above is a blanket ceiling
independent of chunk granularity, so a legitimately-small decoded body sent
as pathologically many tiny chunks could be rejected as "too large" purely
from framing overhead, even though its decoded size is well under
`MAX_BODY_SIZE`. This is a deliberate simplification favoring predictable
memory bounds over supporting that edge case - similar in spirit to the
known `%2F` path-decoding gap in "URL decoding" above.

See `tests/test_http_parser.c` (`test_request_has_chunked_encoding`,
`test_chunked_body_scan`, `test_chunked_body_decode`,
`test_request_is_complete_chunked`, `test_parse_http_request_chunked`) and
`tests/test_connection.c` (`test_handle_readable_chunked_round_trip`,
`test_handle_readable_chunked_grows_buffer`,
`test_handle_readable_chunked_too_large_413`,
`test_handle_readable_chunked_malformed_400`,
`test_handle_readable_chunked_and_content_length_400`).

## multipart/form-data parsing
`multipart_parse_boundary`/`parse_multipart_body`/`multipart_get_part`
(`lib/multipart.c/h`) parse a `multipart/form-data` `Request.body` the same way
`parse_query_string`/`parse_headers` parse the query string/header block - pure
functions over a `const char *` buffer, called explicitly by a handler rather
than run automatically by `parse_http_request` (unlike headers/query, which every
request has; multipart is one specific `Content-Type` a handler opts into
checking for, the same way `app/handlers.c: has_json_content_type` gates
`json_parse` on `req->body`).
- **`multipart_parse_boundary(content_type, boundary_out, size)`** checks
  `content_type` (from `req_get_header(req, "Content-Type")`) is
  `multipart/form-data` (prefix match, case-insensitive) and extracts its
  `boundary=` parameter - bare or double-quoted (RFC 2046 permits either) - into
  `boundary_out`. Returns `0` (not `1`) for a non-multipart `Content-Type`, a
  missing boundary parameter, or a boundary longer than `boundary_out` (capped at
  `MAX_BOUNDARY_LEN`, `app_types.h` - RFC 2046's own 70-character limit on a
  boundary delimiter) can hold - a handler treats any of these identically to a
  malformed request, same as `has_json_content_type` finding no JSON.
- **`parse_multipart_body(body, body_len, boundary, form)`** splits `body` on the
  RFC 2046 delimiters (`"--boundary\r\n"` between parts, `"--boundary--"` after
  the last one) using `memmem` rather than `strstr` throughout - `body` is
  `body_len` raw bytes, not assumed to be a NUL-terminated C string, since a file
  part's payload can contain arbitrary bytes including embedded NULs (this is why
  `parse_http_request`'s `raw_len` fix above exists: `body`/`body_len` here is
  usually `req->body`/`req->content_length`, and `req->body` must actually contain
  every byte the client sent for this to work). Each part's own small header block
  (`Content-Disposition`, optionally `Content-Type`) is parsed with the same
  `extract_param` helper for both `Content-Disposition`'s `name=`/`filename=` and
  `multipart_parse_boundary`'s own `boundary=` - one quoted-or-bare
  parameter-value extractor shared by both call sites. A part with no
  `Content-Disposition name=` is skipped outright (malformed - there's no form
  field to key it by); parts are collected into `form->parts`, bounded by
  `MAX_MULTIPART_PARTS` (`app_types.h`) - extra parts past the cap are dropped
  with a stderr warning rather than overflowing the fixed array, same convention
  as `MAX_ROUTES`/`MAX_HEADERS` elsewhere (`router.c`).
- **`MultipartPart.data` (`app_types.h`) points directly into `body` - it is never
  copied, and it is NOT NUL-terminated.** A `filename` present (non-empty) marks a
  file part; callers must use `data_len`, never `strlen`, to read `data` either
  way, since a plain text field's value happens to not contain a NUL in the
  common case but is still not NUL-terminated at the byte after it (that byte is
  the start of the next delimiter's `\r`, not a `\0`). `multipart_get_part(form,
  name)` looks the array up by `Content-Disposition name=`, mirroring
  `req_get_header`/`req_get_query`'s first-match lookup convention.
- **No copy, no separate `free()`.** Because every `MultipartPart.data` aliases
  the caller-owned `body` buffer, `MultipartForm` itself owns no heap memory - a
  caller frees only what it already owned going in (typically `req->body`, freed
  by `handle_readable` as usual), same as `Request.header_names`/`query_names`
  pointing at fixed arrays needing no separate release.
- See `tests/test_multipart.c` (boundary extraction, field/file-part splitting,
  the embedded-NUL binary-payload case, nameless-part skipping, and the
  `MAX_MULTIPART_PARTS` truncation cap) and `app/handlers.c: handler_upload`
  (`POST /upload`, `app/CLAUDE.md`) for a real usage example.

## application/x-www-form-urlencoded parsing
`parse_urlencoded_body(body, body_len, form)`/`urlencoded_get_field(form, name)`
(`lib/urlencoded.c/h`) parse an `application/x-www-form-urlencoded`
`Request.body` the same way `multipart_parse_boundary`/`parse_multipart_body`
parse a multipart body — a pure function over a `const char *` buffer, called
explicitly by a handler that has already gated on `Content-Type` (one specific
media type a handler opts into checking for, not run automatically by
`parse_http_request` the way header/query parsing is).
- **Same grammar as the query string, different buffer.**
  `application/x-www-form-urlencoded` is byte-for-byte the same
  `key=value&key2=value2` grammar `parse_query_string` already splits out of
  `req->query` — `&` separates pairs, the first `=` in a pair separates key
  from value, a pair with no `=` gets an empty-string value, and both key and
  value are run through `url_decode` (`decode_plus = 1`, `http_parser.c`) —
  but `parse_urlencoded_body` does **not** reuse `parse_query_string` itself,
  because `req->query` (and the `strtok_r`-based local copy
  `parse_query_string` makes of it) is capped at `sizeof(req->query)` (256
  bytes), while a form body is a `Request.body`, bounded only by
  `MAX_BODY_SIZE` (10 MiB) same as any other body. `parse_urlencoded_body`
  instead walks `body`/`body_len` directly with `memchr` to find each `&`/`=`
  boundary and `memcpy`s each segment into a bounded, NUL-terminated
  `UrlEncodedForm` (`app_types.h`) slot before `url_decode`-ing it in place —
  `body` itself is never mutated, mirroring `parse_multipart_body`'s
  `memmem`-based, non-mutating walk over the same kind of buffer.
- **`UrlEncodedForm.field_names`/`field_values`** (`app_types.h`) are a fixed
  `MAX_FORM_FIELDS`-sized array + `field_count`, the same bounded-array-plus-
  count shape as `query_names`/`query_values` — extra pairs past the cap are
  dropped rather than overflowing, same convention as `MAX_QUERY_PARAMS`/
  `MAX_HEADERS` elsewhere. A field value longer than its slot (256 bytes,
  matching `header_values`/`cookie_values`) is truncated rather than growing —
  this is a form *field*, not a file upload (that's what `multipart/form-data`
  above is for).
- **`body` may be `NULL`** (a request with `Content-Length: 0`), treated the
  same as an empty body — `form->field_count` is just left at `0`, not a parse
  error, same convention as `parse_cookies` accepting a `NULL` `Cookie` header.
- **Content-Type gating is the handler's job**, not `parse_urlencoded_body`'s —
  unlike `multipart_parse_boundary` (which has to inspect `Content-Type`
  anyway, to extract the `boundary=` parameter), there's nothing left to
  extract from an `application/x-www-form-urlencoded` `Content-Type` once its
  presence is confirmed, so the check is a small prefix-match helper at the
  app layer (`app/handlers.c: has_urlencoded_content_type`), exactly mirroring
  `has_json_content_type`'s existing convention rather than adding a new one
  to the engine.
- **`urlencoded_get_field(form, name)`** looks the array up by exact name
  (`strcmp`), mirroring `req_get_query`/`multipart_get_part`'s first-match
  lookup convention.
- See `tests/test_urlencoded.c` (splitting/decoding, bare-key/empty-pair
  handling, `NULL`/empty bodies, `MAX_FORM_FIELDS` truncation) and
  `app/handlers.c: handler_form` (`POST /form`, `app/CLAUDE.md`) for a real
  usage example.

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

`res_redirect(res, status, location)` (`response.c`) is a thin composition of
existing primitives, not a new response path: it calls `res_status` (`status`,
or `302` when `status` is `0` - C has no way to omit an argument to signal
"use the default" the way Express's `res.redirect(path)` does), then
`res_set_header(res, "Location", location)`, then `res_send` with a short
`"Redirecting to <location>"` body - so it inherits `res_set_header`'s
truncation-at-256-bytes behavior for an unusually long `location`, same as any
other header value. Callers are responsible for passing a real 3xx code;
`res_redirect` does not validate `status` itself.

`status_text` (`http_parser.c`) covers 27 status codes as of this writing -
notably the 3xx codes `res_redirect` needs (`301`/`302`/`303`/`304`/`307`/`308`)
plus a handful of common 4xx/5xx codes an app or middleware might reasonably
return (`409`/`410`/`415`/`422`/`429`/`501`/`502`/`503`). It's still not
exhaustive (e.g. `418` deliberately falls through to `"Unknown"`, and is
asserted as such in `tests/test_http_parser.c: test_status_text`) - codes are
added here as this project actually needs them, not preemptively.

## Memory lifecycle
- `Connection` (`connection_create`/`connection_close`): one `calloc` per accepted
  fd, freed exactly once in `connection_close`, which also deregisters the fd from
  kqueue and closes the socket first. `connection_create` also `malloc`s
  `conn->in_buf` at that same point (see "Body buffering") - a failed `malloc` for
  either frees whatever did succeed and returns `NULL`, and `accept_connections`
  (`connection.c`) closes the fd without registering the connection rather than
  dereferencing a `NULL` `Connection *`.
- `conn->in_buf` (`connection_create`/`connection_close`/`connection.c: handle_readable`,
  `flush_connection`): one `malloc` per connection (not per request), `realloc`'d
  larger by `handle_readable` to fit an oversized body and back down to `BUF_SIZE`
  by `flush_connection` once idle (see "Body buffering") - freed exactly once, in
  `connection_close`, regardless of its capacity at that point.
- `conn->out_buf` (`response.c: send_with_content_type`): one `malloc` per response,
  built by a handler via `res_send`/`res_json`. Freed in exactly one of two places —
  `flush_connection` once a keep-alive connection finishes writing it, or
  `connection_close` when the connection is torn down (error, `Connection: close`, or
  a write that never finished) — never both, since a connection only ever takes one
  of those two paths. If the `malloc` itself fails, `send_with_content_type` leaves
  `out_len` at 0 and marks the connection for close rather than writing through a
  NULL pointer.
- `req->body` (`http_parser.c: parse_http_request`): one `malloc` per request (see
  "Body buffering"), freed by `handle_readable` (`connection.c`) right after
  `dispatch` returns, on every path - success or a failed parse (`req->body` is
  guaranteed `NULL` on failure via `parse_http_request`'s initial `memset`, so
  `free(NULL)` there is always safe).
- Every other `Request` field is fixed-size (`app_types.h`), never heap-allocated -
  `req->body` is the one exception, and the *why* is covered in "Body buffering".
- `app->connections` (`router.c: app_init`/`connection.c: ensure_connection_capacity`/
  `app_destroy`): one `calloc` at `app_init` (`INITIAL_CONNECTION_TABLE_CAP` slots,
  see "Architecture" above), `realloc`'d larger as needed - never smaller, unlike
  `conn->in_buf`, since a fd this table has already grown to accommodate can be
  reused by a future `accept()` at any time. Freed exactly once, by the new
  `app_destroy` (`connection.h`), which also walks every still-populated slot and
  calls `connection_close` on it first (so no individual `Connection`/`conn->in_buf`/
  `conn->out_buf` is ever leaked by tearing the table down), then closes `kq`/
  `server_fd` if still open. Called from `app/main.c` right after `app_listen`
  returns (clean process teardown on graceful shutdown) and by test teardown
  (`tests/test_connection.c: teardown_test_connection`).

## Graceful shutdown
Graceful shutdown (`connection.c: app_listen`, `app_stop`) coordinates signal handling,
socket draining, and memory teardown without blocking the single event thread:
- **Signal interception**: `app_listen` sets `signal(SIGINT, SIG_IGN)` and
  `signal(SIGTERM, SIG_IGN)` so default termination dispositions are suppressed, and
  registers both signals with kqueue using `EVFILT_SIGNAL`. Signals are delivered
  synchronously as events within `kevent()` — completely async-signal-safe, with no
  signal handlers, pipes, or volatile flags required. A second signal received while
  already draining forces immediate exit.
- **Draining state machine** (`app_stop`): sets `App.is_shutting_down = 1`, drops
  interest in `server_fd` from kqueue and closes it (`server_fd = -1`) so no new
  connections are accepted. It sweeps `app->connections`:
  - Idle keep-alive connections (`in_len == 0 && out_buf == NULL`) are closed immediately.
  - In-flight connections (`in_len > 0 || out_buf != NULL`) have `conn->keep_alive = 0`
    set so they terminate as soon as the current request finishes.
  - Arms a oneshot `EVFILT_TIMER` (ident 2, `SHUTDOWN_TIMEOUT_SECONDS = 5s`) on kqueue
    so slow or stalled clients cannot hold the process indefinitely.
- **Response semantics during drain**: `handle_readable` enforces
  `conn->keep_alive = !request_wants_close(req) && !app->is_shutting_down`. Any response
  built during draining emits `Connection: close` (via `send_with_content_type`), and
  `flush_connection` calls `connection_close` immediately once the bytes are sent.
- **Loop exit and teardown**: once `app->is_shutting_down` is set and
  `app_count_connections(app) == 0` (or the 5-second shutdown timer expires), `app_listen`
  breaks its loop, restores default signal dispositions, and returns. `main.c` then
  calls `app_destroy(&app)`, releasing all remaining memory and sockets.

## Streaming & Chunked Responses
CExpress supports incremental chunked responses and event-loop-driven bounded file streaming:
- **Procedural chunked streaming** (`res_write`, `res_end` in `lib/response.c`):
  - Handlers call `res_write(res, data, len)` to emit chunks and `res_end(res)` to terminate.
  - The first call commits response headers with `Transfer-Encoding: chunked` and any custom
    headers/cookies set on `res`.
  - `conn->out_buf` grows dynamically via `append_to_out_buf` (realloc) up to `MAX_BODY_SIZE`
    (10MB), preserving the architectural invariant that handlers do not perform blocking socket
    I/O during dispatch, keeping response generation 100% unit-testable.
  - For `HEAD` requests, chunk framing and data are suppressed per RFC 7230 3.3.3: only the
    response headers terminating with `\r\n\r\n` are sent.
- **Chunked trailers** (`res_set_trailer` in `lib/response.c`):
  - Handlers stage up to `MAX_RESPONSE_TRAILERS` (8) trailers (e.g. `Server-Timing`).
  - Trailers set before headers are sent declare `Trailer: <names>` in the header block.
  - Forbidden trailer field names (`Transfer-Encoding`, `Content-Length`, `Trailer`) are rejected.
  - Emitted after the terminal `0\r\n` chunk and followed by the final `\r\n`.
- **Bounded file streaming** (`res_send_file` in `lib/response.c`, `flush_connection` in `lib/connection.c`):
  - `res_send_file` stats the target file, validates it is a regular file, commits headers with
    exact `Content-Length`, sets `conn->file_fd`, and records `conn->file_remaining`.
  - `flush_connection` drains headers, then reads up to `STREAM_CHUNK_SIZE` (16KB) at a time into
    `conn->out_buf` and writes to the socket.
  - Bounded memory footprint: large files are never buffered into RAM.
  - Cooperative yielding: flushes up to 64KB (`4 * STREAM_CHUNK_SIZE`) per event-loop turn,
    registering `EVFILT_WRITE` to allow other connections to make progress.
  - Idle timeout (`close_idle_connections`) and shutdown (`app_stop`) check `conn->file_fd >= 0`
    as an in-flight condition so streaming transfers are not severed prematurely.

## Const correctness
`Handler` (`app_types.h`) takes `const Request *`: routing (`match_path`/`match_route`)
is the only code that mutates a `Request` (filling in path params before dispatch);
once a handler runs, the request is read-only for the rest of its lifetime.
