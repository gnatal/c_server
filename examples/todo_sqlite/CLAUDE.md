# examples/todo_sqlite/ — application layer

## Architecture
Thin layer on top of `lib/libcexpress.a`: `main.c` builds one `App`, wires up
SQLite-backed storage (see "Persistence layer" below), registers app-wide
middleware via `app_use`/`app_use_error` (`middlewares.c`), registers routes,
and calls `app_listen` to hand control to the engine's event loop
permanently — nothing here runs after that call. `handlers.c` holds the
route bodies; each one is a terminal function — the engine's `Middleware`
pipeline (`lib/middleware.h`) runs ahead of it, but route handlers
themselves don't participate in chaining (no `next` access), so they call
`res_send`/`res_json`/`res_status` directly, same as before.

This app is a single-resource CRUD example: a Todo list backed by a real
SQL database, chosen to demonstrate the engine's routing, middleware,
JSON, and error-handling machinery doing actual work end-to-end rather than
echoing input back. It deliberately does not exercise every `lib/` feature
(cookies, multipart, chunked streaming, route wildcards) — that coverage
lives in `tests/` and `lib/CLAUDE.md`, not in this app.

**Working directory matters.** The demo opens `public/index.html`, mounts `public/` and defaults the database to
`todos.db`, all relative to the process's working directory. Build it from the repository root with `make demo`
(output: `examples/todo_sqlite/cexpress_demo`, linking `build/lib/libcexpress.a`; the demo Makefile does not track
the library, so rebuild with `make -B -C examples/todo_sqlite` after engine changes) and start it from
`examples/todo_sqlite/` (the Docker image uses `WORKDIR /app` with `public/` copied next to the binary). Started from
anywhere else, `GET /` answers 404 and `app_serve_static` logs `root directory "public" does not exist, not registered`.

Routes (`main.c`):
- `GET /` → `handler_home` — serves the Todo UI (`public/index.html`,
  a single self-contained file, 6,481 bytes) via `res_send_file` (bounded chunk
  streaming, `lib/response.h`).
- `GET /ping` → `handler_ping` (defined inline in `main.c`) — answers `200 pong` (text/plain) with no
  database access and no JSON: the target for connection stress tests
  (`PHASES=ping scripts/stress_test.sh`, `scripts/CLAUDE.md`). `tests/test_ping.c` carries its own copy of the
  same three-line handler, so it does not link any demo code. Like every route it runs the app-wide
  middleware (`mw_logger` unless `QUIET=1`, `mw_body_size_guard`).
- `app_serve_static(&app, "/static", "public")` — a generic
  static-file-serving demo (`lib/CLAUDE.md`, "Behavior reference, Static"),
  unrelated to the Todo UI above, which needs no separate assets.
- The Todo REST API is built on a `Router` (`todo_router`) and mounted at
  `/api/todos` via `app_mount` (`lib/CLAUDE.md`, "Behavior reference, Sub-routers"):
  `GET /` and `GET /:id` (→ `handler_list_todos`/`handler_get_todo`) are
  unprotected; `POST /`, `PUT /:id`, `PATCH /:id`, `DELETE /:id` each
  attach `mw_authenticate` as **per-route** middleware (`router_post_mw`/
  etc), not `router_use` — a `router_use`-registered middleware would gate
  every route on the mount, including the public `GET`s. This is the one
  place in this app that combines a mounted sub-router with per-route
  middleware in the same route set, rather than showing each mechanism in
  isolation the way the previous demo did.

## Persistence layer (`examples/todo_sqlite/db.c`/`examples/todo_sqlite/db.h`/`examples/todo_sqlite/todo_types.h`)
Lives under `examples/todo_sqlite/`, not `lib/` — this is app-specific persistence, not a
generic engine capability (Express itself ships no DB layer either; you'd
bring your own). `lib/` has no SQLite dependency at all except for the
generic worker-lifecycle hook this module consumes (below).

- **Schema**: one `todos` table (`id` `INTEGER PRIMARY KEY AUTOINCREMENT`,
  `title TEXT NOT NULL`, `done INTEGER NOT NULL DEFAULT 0 CHECK (done IN
  (0,1))`, `created_at`/`updated_at TEXT` defaulting to an ISO-8601 UTC
  timestamp via SQLite's `strftime`). Created by `db_open` via `CREATE
  TABLE IF NOT EXISTS`, so re-running it against an existing database is a
  no-op.
- **No connection handle type is exposed.** `Handler` (`lib/app_types.h`)
  takes only `(const Request *, Response *)` — there's no context parameter
  to inject a `Db *` through — so the connection is module-level state in
  `db.c` (`static sqlite3 *g_db`), the same pattern `examples/todo_sqlite/middlewares.c:
  mw_authenticate_set_key`/`mw_authenticate_get_key` already uses for the
  configured API key.
- **Fork safety, and why `db_open`/`db_worker_init` are two separate
  functions.** `lib/cluster.c` may `fork()` worker processes (`WORKERS` >
  1) after `main()` runs, and a `sqlite3*` connection open at fork time
  would end up duplicated into every worker — SQLite's own documentation
  calls sharing one connection across a `fork()` a locking-corruption risk.
  `main()` calls `db_open(path)` once, in the original process, purely to
  fail-fast validate the path and run the schema migration, then
  `db_close()`s it immediately — so no connection exists at the moment
  `app_listen()` might fork. `main()` then registers `db_worker_init` via
  `app_on_worker_start(&app, db_worker_init)` (`lib/connection.h`) — the
  engine's generic per-worker-process init hook, see `lib/CLAUDE.md`
  ("Behavior reference, Workers and fork") for the mechanism itself. `db_worker_init`
  opens this process's own private connection (from the path `db_open`
  already validated and stashed in a module-level buffer) and configures
  `sqlite3_busy_timeout` + `PRAGMA journal_mode=WAL` — needed because
  cluster mode means multiple worker processes hit the same `.db` file
  concurrently; WAL lets a writer wait for another process's lock via the
  busy timeout instead of immediately failing with `SQLITE_BUSY`. Verified
  under real concurrent load: 40 concurrent `POST`s fired at a 4-worker
  cluster (Docker/Alpine, `WORKERS=4`) all landed correctly with no errors
  or corruption. A WAL request SQLite can't honor (e.g. an in-memory `":
  memory:"` path) is silently downgraded rather than erroring, so this
  isn't treated as fatal. Every CRUD function below assumes the connection
  is already open by the time it runs a query — the hook always runs
  before `app_listen_worker`'s event loop starts serving requests — rather
  than each one carrying its own lazy-open check.
- **CRUD contract** (`db.h`): every function returns `-1` on a
  database-layer error (caller responds `500`); the per-id operations
  (`db_get_todo`/`db_replace_todo`/`db_patch_todo`/`db_delete_todo`) return
  `0` when no row matches `id` (caller responds `404`) and `1` on success;
  `db_list_todos`/`db_create_todo` return `0` on success. Every successful
  per-row operation fills the caller's `Todo *out` with the row's current
  values, including whatever `created_at`/`updated_at` SQLite itself
  generated — a handler never constructs those timestamps itself.
- **Parameterized prepared statements throughout** (`sqlite3_bind_*`),
  never string-built SQL — avoids injection, matches this project's
  bounded/no-unsafe-function posture applied to a new domain.
  `db_patch_todo` (PATCH — partial update) binds `NULL` for an unset field
  against `SET title = COALESCE(?, title), done = COALESCE(?, done)`
  rather than building SQL conditionally per combination of fields present.
  `db_replace_todo` (PUT — full replace) always sets both `title` and
  `done`, and returns `0` (not-found) rather than creating a row when `id`
  doesn't exist — PUT replaces an existing resource, it doesn't upsert.
- **`todo_title_is_valid(title)`** is pure (no database touch): non-NULL,
  non-empty, fits `TODO_TITLE_MAX - 1` bytes. Handlers call it before ever
  reaching the database, on both create (required) and patch (only when a
  `title` field is actually present in the request body).
- **`TodoList.items`/`count`** (`todo_types.h`) is the same bounded-array-
  plus-count shape as `MultipartForm`/`UrlEncodedForm` (`lib/app_types.h`):
  `db_list_todos` truncates past `TODO_LIST_MAX` (256) rows rather than
  growing unbounded (there's no pagination - a bounded list is a deliberate
  demo-sized simplification, not a full solution for large tables). Unlike
  `MAX_MULTIPART_PARTS`/`MAX_QUERY_PARAMS` elsewhere in this project, which
  cap a single anomalous *request*, this cap is a function of total stored
  rows - once a table naturally grows past it, truncation is the steady
  state and would re-fire on every single `GET /api/todos`. The stderr
  warning therefore fires once per process (`static int warned` in
  `db_list_todos`), not per call: found via `scripts/stress_test.sh`, where
  a per-call `fprintf` under high request rates was slow enough to dominate
  request latency and flood the terminal (a naive per-call warning turned
  ordinary read traffic into ~345,000 stderr writes in 15 seconds).

## Handlers (`handlers.c`)
- **`write_todo(yyjson_mut_doc *, const Todo *)`** builds the JSON object every todo-returning
  handler shares (`{"id", "title", "done", "created_at", "updated_at"}`) in a yyjson
  mutable document (`lib/vendor/yyjson`, included by `cexpress.h`). `id` is a JSON integer
  (`yyjson_mut_obj_add_int`, exact for any 64-bit id), `done` a boolean, the rest strings
  (escaped on write, so a title containing `"` or a newline round-trips). The string values are
  borrowed from the stack `Todo`, not copied, which is safe because the document is serialized before
  the handler returns. `send_todo_json` and `handler_list_todos` create the document over the
  connection arena (`arena_yyjson_alc(&res->conn->arena)`), serialize with `yyjson_mut_write`, hand the
  text to `res_json`, `free` it (the serialized string is libc-malloc'd even for an arena document)
  and call `yyjson_mut_doc_free` (a no-op for an arena document). A `NULL` result answers 500.
  History: the first version used `json_new_*` + `json_stringify` (a `%g` bug printed ids
  above 999,999 as `1.23457e+06`), then an in-repo streaming `JsonWriter`; both are gone.
- **`parse_id_param(req, &id)`** parses the `:id` path param
  (`req_get_param`) as a bounded, non-negative `long long` via `strtoll`,
  rejecting anything malformed (trailing garbage, negative, empty,
  missing) with `400` before ever touching the database.
- **`handler_create_todo`** (`POST /api/todos`) requires a
  `application/json` `Content-Type` (`has_json_content_type`, same
  prefix-match convention this handler set already used for the old
  `/echo/json`/`/users/:id` demos) and a valid, non-empty `title`; responds
  `201` + the created row on success.
- **`handler_replace_todo`** (`PUT /api/todos/:id`) requires both `title`
  and `done` in the body (full replace) and `404`s if `id` doesn't exist.
- **`handler_patch_todo`** (`PATCH /api/todos/:id`) treats `title`/`done`
  as independently optional — a `title` key that's present but invalid
  (empty/oversized) is still rejected with `400`; a `done` value that's
  present but not a JSON boolean is treated as `false` (`json_as_bool`'s
  default), the same lenient-degrade convention `handler_list_todos`'s
  `?done=` query filter already uses for an unrecognized value, rather than
  a `400`.
- **`handler_delete_todo`** responds `204 No Content` with an empty body,
  matching the previous demo's `handler_delete_user` convention.
- **Error shape**: every handler-level failure responds via a shared
  `send_error(res, status, message)` helper that writes `{"error": "..."}`
  with yyjson over the connection arena (same shape `error_handler_json`, `middlewares.c`,
  produces for middleware-level failures, there with a `NULL`-allocator yyjson document; both escape the message) — handlers have no `MiddlewareChain *` (they
  are the terminal node of the pipeline, `lib/CLAUDE.md`, "Behavior reference, Middleware"), so they can't call `chain_error` and build the response
  directly instead, same as the previous demo's `handler_update_user`/
  `handler_patch_user`.

## Frontend (`examples/todo_sqlite/public/index.html`)
A single self-contained page (inline `<style>`+`<script>`, no build step,
no external assets) — the served page *is* the Todo UI, not a static-asset
demo. Vanilla `fetch()` calls against `/api/todos`; an API-key `<input>`
attaches `Authorization: Bearer <key>` to mutating requests only (`POST`/
`PUT`/`PATCH`/`DELETE`), so `mw_authenticate` (`examples/todo_sqlite/middlewares.c`) is
reachable interactively from the browser, not just via `curl`. Failed
requests surface the server's `{"error": "..."}"` message inline rather
than failing silently. `examples/todo_sqlite/public/style.css` and
`examples/todo_sqlite/public/docs/index.html` are unrelated leftovers from the previous
static-file demo, still reachable via the generic `/static` mount above.

## Runtime Configuration
- Server listening port is initialized in `main.c` from the `PORT` environment variable
  (bounded between 1 and 65535) with fallback to `DEFAULT_PORT` (8080), stored in `app.config.port`.
- API authentication secret is configured via `API_KEY` environment variable in `main.c`
  or dynamically via `mw_authenticate_set_key`.
- `WORKERS` (`N`, or `auto` = one per CPU core), `QUIET=1` (no access log) and `TLS_CERT` + `TLS_KEY` (both required to
  serve HTTPS) are read in `main.c` too.
- SQLite database path is configured via `TODO_DB_PATH` (default `"todos.db"`,
  relative to the server's working directory) — see "Persistence layer" above
  for why it's read once in `main.c` and handed to `db_open`/stashed for
  `db_worker_init` rather than opened directly.

## Data flow / state
A `Handler` receives a `const Request *` (already routed and, for pattern
routes like `/api/todos/:id`, already populated with path params — see
`req_get_param`) and a `Response *` it must call
`res_send`/`res_json`/`res_status` on to produce output. Handlers hold no
state of their own across requests and don't touch sockets or the database
connection directly — persistence goes through `examples/todo_sqlite/db.c`'s functions,
socket I/O happens in `lib/connection.c`, both outside this layer.

Every handler that builds a JSON response creates a yyjson mutable document over the connection arena,
serializes it once with `yyjson_mut_write`, and `free`s that string on every path (the response layer copies
the bytes, so freeing right after `res_json` is safe). A request body is parsed with `yyjson_read_opts` over the same
arena; strings read from it (`yyjson_get_str`) point into that document, so they are passed to the
database layer before `yyjson_doc_free` (a no-op for an arena document, but the ordering keeps the code valid for a
`NULL`-allocator document too). The arena is reset when the keep-alive response has been flushed, so nothing
allocated from it may be kept in a module-level variable. The handlers parse the body with `strlen(req->body)` rather than
`req->content_length`, so a body containing a NUL byte is cut short there (invalid JSON either way);
see `lib/CLAUDE.md` ("Memory model", "Ownership") for the arena rules and
`lib/examples/cookbook.c` for the same patterns as small tested recipes.