# app/ — application layer

## Architecture
Thin layer on top of `lib/libcserver.a`: `main.c` builds one `App`, registers routes
via `app_get`/`app_post` (each binds a method + path pattern to a `Handler`), and
calls `app_listen` to hand control to the engine's event loop permanently — nothing
here runs after that call. `handlers.c` holds the actual route bodies; each one is a
terminal function (no middleware/`next()` chaining exists in the engine).

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
