# app/ — application layer

## Architecture
Thin layer on top of `lib/libcexpress.a`: `main.c` builds one `App`, registers
app-wide middleware via `app_use`/`app_use_error` (`middlewares.c`), registers
routes via `app_get`/`app_post` (each binds a method + path pattern to a
`Handler`), and calls `app_listen` to hand control to the engine's event loop
permanently — nothing here runs after that call. `handlers.c` holds the actual
route bodies; each one is a terminal function — the engine's `Middleware` pipeline
(`lib/middleware.h`) runs ahead of it, but route handlers themselves don't
participate in chaining (no `next` access), so they still just call
`res_send`/`res_json`/`res_status` directly as before.

`middlewares.c` holds this app's concrete middleware/error-handler instances.
`main.c` registers `mw_logger` and `mw_body_size_guard` app-wide via `app_use` (they
run ahead of every request), registers `error_handler_json` as the error handler,
and attaches `mw_authenticate` two different ways to demonstrate both middleware
scoping mechanisms in `lib/`:
- *Per-route*, on just `POST /echo/json`, via `app_post_mw(&app, "/echo/json",
  handler_echo_json, (Middleware[]){mw_authenticate}, 1)` — every other
  top-level route (`/`, `/users/:id`, `/echo`) is reachable without a token,
  since `app_get`/`app_post` register a route with zero route-level middleware.
- *Router-scoped*, on an `api_router` (`Router`, built with `router_init`/
  `router_get`/`router_use`) mounted at `/api` via `app_mount(&app, "/api",
  &api_router)` — `router_use(&api_router, mw_authenticate)` makes every route
  on that router require a token as a group, rather than repeating
  `app_post_mw`'s middleware list on each one. `router_get(&api_router,
  "/status", handler_api_status)` and `router_get(&api_router, "/users/:id",
  handler_get_user)` become `GET /api/status` and `GET /api/users/:id` once
  mounted — the latter reuses `handler_get_user` from the top-level
  `/users/:id` route to show the same `Handler` works whether reached directly
  or through a mount, path params included. Because `app_mount` implements
  router-level middleware via `app_use_prefix(app, "/api", mw_authenticate)`
  (`lib/CLAUDE.md`, "Sub-router mounting"), auth is enforced for *any* request
  under `/api` — including `GET /api/nope`, which 401s rather than 404ing,
  since the prefix-scoped middleware runs ahead of route dispatch.
  `router_put`/`router_patch`/`router_delete` register `handler_update_user`/
  `handler_patch_user`/`handler_delete_user` against that same `/users/:id`
  pattern (`PUT`/`PATCH`/`DELETE /api/users/:id`) — since `match_route`
  requires an exact method match on top of the path match (`lib/CLAUDE.md`,
  "Deny-by-default routing"), these coexist with the `GET` on the identical
  path without conflicting, and all three inherit `mw_authenticate` from the
  same `router_use` call rather than needing their own `_mw` middleware list.
  `handler_delete_user` responds `204 No Content` with an empty body, the
  one handler in this app that doesn't call `res_json`/plain `res_send` with
  a non-empty body.
- **Content-Type-gated body validation:** `handler_update_user` and
  `handler_patch_user` (`app/handlers.c`) both call a shared
  `has_json_content_type(req)` helper — `req_get_header(req, "Content-Type")`
  (`lib/http_parser.c`) prefix-matched (case-insensitively) against
  `"application/json"`, so a `; charset=...` suffix doesn't break the match —
  before attempting `json_parse` on `req->body`. A declared-JSON body that
  fails to parse gets a `400` with the same `{"error": "..."}` shape
  `handler_echo_json` already used for `POST /echo/json`; a body with no
  Content-Type or a different one skips validation entirely and the handler
  behaves exactly as before (id-only response, body ignored) — this is
  deliberate: without the gate, a client sending plain text or form data to
  `PUT/PATCH /api/users/:id` would get an incorrect `400` for not being JSON
  it never claimed to be.
- `GET /search` (`handler_search`) echoes every query-string param back as a JSON
  object (e.g. `?name=natal&age=32` -> `{"name":"natal","age":"32"}`), built via
  the JSON builder API (`json_new_object`/`json_new_string`/`json_object_set`,
  `lib/json/json.h`) over `req->query_names`/`query_values`
  (`req->query_count` entries, populated by `parse_query_string`,
  `lib/http_parser.c`) rather than `req_get_query` - that accessor is for
  looking up one known key, not iterating all of them. Every value comes back
  as a JSON string with no type coercion, but already URL-decoded (`lib/CLAUDE.md`,
  "URL decoding"/"Query-string parsing"). `json_object_set`
  takes ownership of the `JsonValue *` it's given either way (attached on
  success, freed on failure), so the loop doesn't need to check
  `json_new_string`'s return before passing it in.
- `POST /upload` (`handler_upload`) demonstrates `multipart/form-data` parsing
  (`lib/CLAUDE.md`, "multipart/form-data parsing"): `multipart_parse_boundary`
  (`lib/multipart.h`) gates on `Content-Type` the same way `has_json_content_type`
  gates JSON validation, responding `400` when it's missing/not multipart/has no
  boundary, then `parse_multipart_body` fills a `MultipartForm` the handler walks
  to build a JSON echo - a plain field becomes a JSON string of its value, a file
  part (non-empty `filename`) becomes `{"filename", "content_type", "size"}`
  instead of its raw bytes, since `MultipartPart.data` isn't NUL-terminated and a
  file's payload isn't valid to embed as a JSON string as-is. The one `memcpy` in
  this handler (copying a field's value into a bounded, NUL-terminated stack
  buffer before `json_new_string`, which expects a C string) truncates past 255
  bytes - a deliberate demo-sized cap, not an engine limit. Every file part is
  also written to disk at a fixed path (`temp.txt`, in the server's working
  directory, overwritten on each request) via `save_part_to_temp_file` - purely
  to exercise upload handling by hand, not a real storage feature. The path is a
  hardcoded constant, never `part->filename`, so no client-controlled name ever
  reaches the filesystem; a successful write adds `"saved_as": "temp.txt"` to
  that part's JSON entry, a failed one (`fopen`/`fwrite` error) is silently
  omitted rather than failing the whole request.
- `POST /form` (`handler_form`) demonstrates `application/x-www-form-urlencoded`
  parsing (`lib/CLAUDE.md`, "application/x-www-form-urlencoded parsing"): the
  static `has_urlencoded_content_type(req)` helper gates on `Content-Type`
  (prefix match, case-insensitive, same convention as `has_json_content_type`),
  responding `400` when it's missing or a different media type, then
  `parse_urlencoded_body` (`lib/urlencoded.h`) fills a `UrlEncodedForm` the
  handler walks to build a JSON echo — the same shape `GET /search` builds
  from `req->query_names`/`query_values`, since both share the same
  `key=value&key2=value2` grammar, just carried in the body instead of the URL.
- `GET /files/*` (`handler_files`) demonstrates a trailing route wildcard
  (`lib/CLAUDE.md`, "Route wildcards"): one registration answers any path under
  `/files/`, echoing `req->path` back rather than actually serving a file — there's
  still no static file server (`pending.txt`).
- `GET /login` / `GET /whoami` / `GET /logout` demonstrate cookies
  (`lib/CLAUDE.md`, "Cookies"): `handler_login` calls `res_set_cookie(res,
  "session", "demo-session-token", &options)` (`lib/response.h`) with
  `HttpOnly` + `SameSite=Lax` + a one-hour `Max-Age` (demo choices, not
  requirements of the API); `handler_whoami` reads it back via
  `req_get_cookie(req, "session")` (`lib/http_parser.h`); `handler_logout`
  calls `res_clear_cookie(res, "session", "/")` to expire it — the `"/"` must
  match the `Path` the cookie was set with (`options.path` above, also `"/"`)
  for a browser to actually delete it rather than leaving an orphaned cookie
  scoped to a `Path` nothing clears anymore.
- `mw_logger` calls `chain_next` first and logs `METHOD PATH -> STATUS` after it
  returns, once the rest of the pipeline has produced a final `res->status`.
- `mw_body_size_guard` rejects any request whose `req->content_length` exceeds this
  app's own `MAX_APP_BODY_SIZE` (`app/middlewares.h`, set equal to the engine's hard
  per-connection `MAX_BODY_SIZE` cap - 10 MiB, `lib/CLAUDE.md`, "Body buffering" -
  so a file upload via `POST /upload` isn't rejected by this app-level policy
  before the engine's own cap would apply anyway) via `chain_error(chain, 400,
  ...)` instead of calling `chain_next`. Since this runs as middleware (after the
  engine has already fully received the body), this guard only ever rejects what
  the engine's own `Content-Length` check (413, `lib/CLAUDE.md`, "Body buffering")
  would already have rejected - it exists as a worked example of an app enforcing
  its own (potentially tighter) body-size policy independently of the engine's,
  not because the two currently differ.
- `mw_authenticate` enforces Bearer token authentication via `Authorization: Bearer <token>`,
  read via `req_get_header(req, "Authorization")` (`lib/http_parser.c`) rather than an
  ad-hoc `strstr` scan over `req->headers` - `req_get_header`'s lookup is
  case-insensitive per RFC 7230 and already trims/isolates the value, so
  `mw_authenticate` only needs to strip the literal `"Bearer "` prefix itself.
  Tokens are verified against the configured key using constant-time comparison
  (`keys_match`) to avoid timing side-channels. The expected key is resolved hierarchically:
  explicit setter (`mw_authenticate_set_key`) → `API_KEY` environment variable → default
  fallback key. Being route-scoped rather than app-wide (`lib/CLAUDE.md`), it only
  runs on the route(s) it's attached to.
- `error_handler_json` is the app's single `ErrorHandler`: anything routed to
  `chain_error` (such as body-size or authentication violations) comes back as
  `{"error": "..."}` instead of the engine's default plain-text fallback.

## Runtime Configuration
- Server listening port is initialized in `main.c` from the `PORT` environment variable
  (bounded between 1 and 65535) with fallback to `DEFAULT_PORT` (8080), stored in `app.config.port`.
- API authentication secret is configured via `API_KEY` environment variable in `main.c`
  or dynamically via `mw_authenticate_set_key`.

## Data flow / state
A `Handler` receives a `const Request *` (already routed and, for pattern routes
like `/users/:id`, already populated with path params — see `req_get_param`) and a
`Response *` it must call `res_send`/`res_json`/`res_status` on to produce output.
Handlers hold no state across requests and don't touch sockets directly; building
the response bytes (`response.c`) and writing them to the wire
(`connection.c: flush_connection`) both happen outside this layer.

`handler_echo_json` is the one handler that allocates: it owns the `JsonValue *`
from `json_parse` and the `char *` from `json_stringify` for the duration of the
call and releases both (`json_free`, `free`) before returning — see
`lib/json/CLAUDE.md` for the json library's own memory rules.
