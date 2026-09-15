# app/ — application layer

## Architecture
Thin layer on top of `lib/libcserver.a`: `main.c` builds one `App`, registers
app-wide middleware via `app_use`/`app_use_error` (`middlewares.c`), registers
routes via `app_get`/`app_post` (each binds a method + path pattern to a
`Handler`), and calls `app_listen` to hand control to the engine's event loop
permanently — nothing here runs after that call. `handlers.c` holds the actual
route bodies; each one is a terminal function — the engine's `Middleware` pipeline
(`lib/middleware.h`) runs ahead of it, but route handlers themselves don't
participate in chaining (no `next` access), so they still just call
`res_send`/`res_json`/`res_status` directly as before.

`middlewares.c` holds this app's concrete middleware/error-handler instances,
registered in `main.c` in this order — `mw_logger`, `mw_body_size_guard`, `mw_authenticate`,
then `error_handler_json` as the error handler:
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
  fallback key.
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
