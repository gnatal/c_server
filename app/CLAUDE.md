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
- `GET /search` (`handler_search`) echoes every query-string param back as a JSON
  object (e.g. `?name=natal&age=32` -> `{"name":"natal","age":"32"}`), built via
  the JSON builder API (`json_new_object`/`json_new_string`/`json_object_set`,
  `lib/json/json.h`) over `req->query_names`/`query_values`
  (`req->query_count` entries, populated by `parse_query_string`,
  `lib/http_parser.c`) rather than `req_get_query` - that accessor is for
  looking up one known key, not iterating all of them. Every value comes back
  as a JSON string with no type coercion, and unescaped/undecoded exactly as
  the client sent it (`lib/CLAUDE.md`, "Query-string parsing"). `json_object_set`
  takes ownership of the `JsonValue *` it's given either way (attached on
  success, freed on failure), so the loop doesn't need to check
  `json_new_string`'s return before passing it in.
- `GET /files/*` (`handler_files`) demonstrates a trailing route wildcard
  (`lib/CLAUDE.md`, "Route wildcards"): one registration answers any path under
  `/files/`, echoing `req->path` back rather than actually serving a file — there's
  still no static file server (`pending.txt`).
- `mw_logger` calls `chain_next` first and logs `METHOD PATH -> STATUS` after it
  returns, once the rest of the pipeline has produced a final `res->status`.
- `mw_body_size_guard` rejects any request whose `req->content_length` exceeds this
  app's own `MAX_APP_BODY_SIZE` (4096, independent of and tighter than the engine's
  hard per-connection `BUF_SIZE` cap) via `chain_error(chain, 400, ...)` instead of
  calling `chain_next`.
- `mw_authenticate` enforces Bearer token authentication via `Authorization: Bearer <token>`.
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
