/*
 * CExpress cookbook: one small, tested recipe per common task. Copy the recipe closest to what
 * you need. Every handler has the same shape:
 *
 *     static void name(const Request *req, Response *res);
 *
 * and must produce exactly one response with res_send / res_json / res_send_bytes / res_redirect /
 * res_send_file (or res_write ... res_end). Handlers are terminal: they cannot call chain_next or
 * chain_error (only Middleware can). A second res_send in the same request replaces the first.
 *
 * DON'T (each of these is a real bug pattern, see lib/CLAUDE.md "Ownership"):
 *  1. free() or keep anything reached through `req`: it dies when the handler returns. That covers
 *     req->body and every pointer from req_get_param / req_get_query / req_get_header / req_get_cookie.
 *  2. strlen(req->body) for binary bodies: use req->content_length (bodies may hold NUL bytes).
 *  3. json_free() a parsed tree before you have finished using strings borrowed from it
 *     (json_as_string returns a pointer INTO the tree).
 *  4. use jw_data() after jw_free(); or forget jw_free() on the error path.
 *  5. call chain_next() from a handler, or in a middleware call it more than once or after responding.
 *  6. open a database/socket in main() and then fork workers: open it in an app_on_worker_start hook.
 *  7. build JSON with snprintf("%s"): the text is not escaped. Use JsonWriter.
 *
 * Limits (excess is truncated or dropped, never overflowed): 32 routes, 16 app middlewares,
 * 8 per-route middlewares, 8 path params (value 63 chars), 16 query params (name/value 63 chars),
 * 32 request headers (value 255 chars), 16 cookies (value 255 chars), 16 response headers,
 * 16 Set-Cookie lines, request body 10 MiB, request headers 8 KiB.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "cexpress.h"
#include "cookbook.h"

/* ---------------------------------------------------------------------------------------------
 * RECIPE 0 - JSON reply helper. Used by most recipes below.
 * Finishes a JsonWriter into a response: 500 if the writer failed, otherwise `status` + body.
 * Always frees the writer.
 * ------------------------------------------------------------------------------------------- */
static void send_json(Response *res, const int status, JsonWriter *w) {
    if (!jw_ok(w)) {
        res_status(res, 500);
        res_json(res, "{\"error\":\"json encoding failed\"}");
    } else {
        res_status(res, status);
        res_json(res, jw_data(w));
    }
    jw_free(w);
}

/* Sends {"error": message} with `status`. */
static void send_error(Response *res, const int status, const char *message) {
    JsonWriter w;
    jw_init(&w);
    jw_object_begin(&w);
    jw_key(&w, "error");
    jw_string(&w, message);
    jw_object_end(&w);
    send_json(res, status, &w);
}

/* ---------------------------------------------------------------------------------------------
 * RECIPE 1 - plain text.            GET /hello   ->  200 "Hello, world"
 * ------------------------------------------------------------------------------------------- */
static void recipe_hello(const Request *req, Response *res) {
    (void)req;
    res_send(res, "Hello, world");
}

/* ---------------------------------------------------------------------------------------------
 * RECIPE 2 - path parameter parsed as an integer, 400 on bad input.
 *   GET /square/:n   ->  {"n":7,"square":49}      GET /square/abc -> 400
 * ------------------------------------------------------------------------------------------- */
static void recipe_square(const Request *req, Response *res) {
    const char *n_text = req_get_param(req, "n"); /* never NULL: the route pattern names it */
    char *end = NULL;
    errno = 0;
    const long n = strtol(n_text, &end, 10);
    if (n_text[0] == '\0' || *end != '\0' || errno != 0 || n < -1000000 || n > 1000000) {
        send_error(res, 400, "n must be an integer between -1000000 and 1000000");
        return;
    }
    JsonWriter w;
    jw_init(&w);
    jw_object_begin(&w);
    jw_key(&w, "n");      jw_int(&w, n);
    jw_key(&w, "square"); jw_int(&w, n * n);
    jw_object_end(&w);
    send_json(res, 200, &w);
}

/* ---------------------------------------------------------------------------------------------
 * RECIPE 3 - query parameter with a default.
 *   GET /greet?name=Ada  ->  "Hello, Ada"        GET /greet  ->  "Hello, stranger"
 * req_get_query returns NULL when the key is absent; the value is already percent-decoded.
 * ------------------------------------------------------------------------------------------- */
static void recipe_greet(const Request *req, Response *res) {
    const char *name = req_get_query(req, "name");
    char body[128];
    snprintf(body, sizeof(body), "Hello, %s", name != NULL && name[0] != '\0' ? name : "stranger");
    res_send(res, body);
}

/* ---------------------------------------------------------------------------------------------
 * RECIPE 4 - JSON request body in, JSON response out.
 *   POST /notes  {"title":"buy milk"}  ->  201 {"id":1,"title":"buy milk"}
 * Checks Content-Type, parses, validates, and frees the tree only AFTER the response is built
 * (`title` points into the tree).
 * ------------------------------------------------------------------------------------------- */
static void recipe_create_note(const Request *req, Response *res) {
    const char *content_type = req_get_header(req, "Content-Type");
    if (content_type == NULL || strncasecmp(content_type, "application/json", 16) != 0) {
        send_error(res, 415, "expected application/json");
        return;
    }

    char err[128];
    JsonValue *body = json_parse(req->body, err, sizeof(err)); /* req->body is NUL-terminated */
    if (body == NULL) {
        send_error(res, 400, err);
        return;
    }

    const char *title = json_as_string(json_object_get(body, "title"), NULL);
    if (title == NULL || title[0] == '\0' || strlen(title) > 255) {
        json_free(body);
        send_error(res, 400, "title is required (1-255 characters)");
        return;
    }

    JsonWriter w;
    jw_init(&w);
    jw_object_begin(&w);
    jw_key(&w, "id");    jw_int(&w, 1);
    jw_key(&w, "title"); jw_string(&w, title);
    jw_object_end(&w);
    json_free(body); /* safe now: the writer copied the title */
    send_json(res, 201, &w);
}

/* ---------------------------------------------------------------------------------------------
 * RECIPE 5 - JSON array of objects (list endpoint), with a bounded query parameter.
 *   GET /numbers?count=3  ->  [{"i":0,"even":true},{"i":1,"even":false},{"i":2,"even":true}]
 * ------------------------------------------------------------------------------------------- */
static void recipe_numbers(const Request *req, Response *res) {
    const char *count_text = req_get_query(req, "count");
    long count = count_text != NULL ? strtol(count_text, NULL, 10) : 3;
    if (count < 0 || count > 100) {
        send_error(res, 400, "count must be between 0 and 100");
        return;
    }
    JsonWriter w;
    jw_init(&w);
    jw_array_begin(&w);
    for (long i = 0; i < count; i++) {
        jw_object_begin(&w);
        jw_key(&w, "i");    jw_int(&w, i);
        jw_key(&w, "even"); jw_bool(&w, i % 2 == 0);
        jw_object_end(&w);
    }
    jw_array_end(&w);
    send_json(res, 200, &w);
}

/* ---------------------------------------------------------------------------------------------
 * RECIPE 6 - middleware. Three kinds, all with the same signature.
 * A middleware either calls chain_next(chain) to continue, or ends the request itself with
 * chain_error(chain, status, message) / res_send. Never both.
 * ------------------------------------------------------------------------------------------- */

/* 6a. Decorate and continue (app-wide via app_use): adds a response header to every request. */
static void mw_request_id(const Request *req, Response *res, MiddlewareChain *chain) {
    (void)req;
    res_set_header(res, "X-Request-Id", "req-1");
    chain_next(chain);
}

/* 6b. Guard (per-route or prefix-scoped): reject unless the X-Token header matches. */
static void mw_require_token(const Request *req, Response *res, MiddlewareChain *chain) {
    (void)res;
    const char *token = req_get_header(req, "X-Token");
    if (token == NULL || strcmp(token, "secret") != 0) {
        chain_error(chain, 401, "missing or invalid X-Token");
        return;
    }
    chain_next(chain);
}

/* 6c. Wrap: code after chain_next runs once the handler has finished, so res->status is final. */
static int g_last_status_seen;
static void mw_record_status(const Request *req, Response *res, MiddlewareChain *chain) {
    (void)req;
    chain_next(chain);
    g_last_status_seen = res->status;
}

/* 6d. The single app-wide error handler: every chain_error lands here (last app_use_error wins). */
static void error_handler_json(const int status, const char *message, const Request *req, Response *res) {
    (void)req;
    send_error(res, status, message);
}

static void recipe_admin_stats(const Request *req, Response *res) {
    (void)req;
    res_json(res, "{\"users\":3}");
}

static void recipe_create_item(const Request *req, Response *res) {
    (void)req;
    res_status(res, 201);
    res_json(res, "{\"created\":true}");
}

/* ---------------------------------------------------------------------------------------------
 * RECIPE 7 - sub-router mounted under a prefix.
 *   GET /v1/notes -> []          GET /v1/notes/7 -> {"id":7}
 * Routes are registered relative to the mount point. Mount before app_listen; the Router may be a
 * stack local (app_mount copies what it needs).
 * ------------------------------------------------------------------------------------------- */
static void recipe_list_notes(const Request *req, Response *res) {
    (void)req;
    res_json(res, "[]");
}

static void recipe_get_note(const Request *req, Response *res) {
    JsonWriter w;
    jw_init(&w);
    jw_object_begin(&w);
    jw_key(&w, "id");
    jw_string(&w, req_get_param(req, "id"));
    jw_object_end(&w);
    send_json(res, 200, &w);
}

/* ---------------------------------------------------------------------------------------------
 * RECIPE 8 - cookies.
 *   GET /login   -> Set-Cookie: session=abc123; Path=/; Max-Age=3600; HttpOnly; SameSite=Lax
 *   GET /whoami  -> "session=abc123" (from the Cookie request header)
 *   GET /logout  -> expires the cookie (same Path it was set with)
 * ------------------------------------------------------------------------------------------- */
static void recipe_login(const Request *req, Response *res) {
    (void)req;
    const CookieOptions options = { .max_age = 3600, .path = "/", .domain = NULL,
                                    .http_only = 1, .secure = 0, .same_site = COOKIE_SAMESITE_LAX };
    res_set_cookie(res, "session", "abc123", &options); /* options may be NULL: session cookie */
    res_send(res, "logged in");
}

static void recipe_whoami(const Request *req, Response *res) {
    const char *session = req_get_cookie(req, "session");
    if (session == NULL) {
        res_status(res, 401);
        res_send(res, "not logged in");
        return;
    }
    char body[300];
    snprintf(body, sizeof(body), "session=%s", session);
    res_send(res, body);
}

static void recipe_logout(const Request *req, Response *res) {
    (void)req;
    res_clear_cookie(res, "session", "/");
    res_send(res, "logged out");
}

/* ---------------------------------------------------------------------------------------------
 * RECIPE 9 - HTML form (application/x-www-form-urlencoded).
 *   POST /form  body: name=Ada+L&age=36  ->  "Hello, Ada L (36)"
 * The parser copies and decodes into the form struct; req->body is untouched.
 * ------------------------------------------------------------------------------------------- */
static void recipe_form(const Request *req, Response *res) {
    const char *content_type = req_get_header(req, "Content-Type");
    if (content_type == NULL || strncasecmp(content_type, "application/x-www-form-urlencoded", 33) != 0) {
        res_status(res, 415);
        res_send(res, "expected application/x-www-form-urlencoded");
        return;
    }
    UrlEncodedForm form;
    parse_urlencoded_body(req->body, (size_t)req->content_length, &form);
    const char *name = urlencoded_get_field(&form, "name");
    const char *age = urlencoded_get_field(&form, "age");
    if (name == NULL || name[0] == '\0') {
        res_status(res, 400);
        res_send(res, "name is required");
        return;
    }
    char body[400];
    snprintf(body, sizeof(body), "Hello, %s (%s)", name, age != NULL ? age : "?");
    res_send(res, body);
}

/* ---------------------------------------------------------------------------------------------
 * RECIPE 10 - file upload (multipart/form-data).
 *   POST /upload  part "file" (filename=a.txt) -> {"filename":"a.txt","bytes":5}
 * Part data points INTO req->body, is NOT NUL-terminated, and may hold NUL bytes: use data_len.
 * ------------------------------------------------------------------------------------------- */
static void recipe_upload(const Request *req, Response *res) {
    char boundary[MAX_BOUNDARY_LEN];
    if (!multipart_parse_boundary(req_get_header(req, "Content-Type"), boundary, sizeof(boundary))) {
        send_error(res, 415, "expected multipart/form-data");
        return;
    }
    MultipartForm form;
    parse_multipart_body(req->body, (size_t)req->content_length, boundary, &form);
    const MultipartPart *file = multipart_get_part(&form, "file");
    if (file == NULL || file->filename[0] == '\0') {
        send_error(res, 400, "missing file part");
        return;
    }
    JsonWriter w;
    jw_init(&w);
    jw_object_begin(&w);
    jw_key(&w, "filename"); jw_string(&w, file->filename);
    jw_key(&w, "bytes");    jw_int(&w, (long long)file->data_len);
    jw_object_end(&w);
    send_json(res, 200, &w);
}

/* ---------------------------------------------------------------------------------------------
 * RECIPE 11 - redirect, 204 No Content, custom header + status.
 * ------------------------------------------------------------------------------------------- */
static void recipe_old_path(const Request *req, Response *res) {
    (void)req;
    res_redirect(res, 301, "/hello"); /* status 0 means 302 */
}

/* Redirect to a caller-supplied target: allow only same-site absolute paths ("/x", not "//host" or
 * "/\\host"), otherwise this is an open redirect. (The engine already refuses CR/LF in Location.)
 *   GET /go?next=/home -> 302 Location: /home        GET /go?next=//evil.example -> 400 */
static void recipe_go(const Request *req, Response *res) {
    const char *next = req_get_query(req, "next");
    if (next == NULL || next[0] != '/' || next[1] == '/' || next[1] == '\\') {
        send_error(res, 400, "next must be a path on this site");
        return;
    }
    res_redirect(res, 302, next);
}

static void recipe_delete_thing(const Request *req, Response *res) {
    (void)req;
    res_status(res, 204);
    res_send(res, "");
}

static void recipe_created(const Request *req, Response *res) {
    (void)req;
    res_set_header(res, "Location", "/things/9");
    res_status(res, 201);
    res_json(res, "{\"id\":9}");
}

/* ---------------------------------------------------------------------------------------------
 * RECIPE 12 - chunked streaming response (size unknown up front).
 *   GET /stream -> "part 0\npart 1\npart 2\n" as three chunks
 * Set headers first: the first res_write commits them. Output is buffered in the connection
 * (bounded by MAX_BODY_SIZE); the handler never blocks on the socket.
 * ------------------------------------------------------------------------------------------- */
static void recipe_stream(const Request *req, Response *res) {
    (void)req;
    res_set_header(res, "Content-Type", "text/plain");
    for (int i = 0; i < 3; i++) {
        char line[32];
        const int n = snprintf(line, sizeof(line), "part %d\n", i);
        res_write(res, line, (size_t)n);
    }
    res_end(res);
}

/* ---------------------------------------------------------------------------------------------
 * RECIPE 13 - per-worker resource (database, cache, anything with OS-level state).
 * fork() copies main()'s memory into every cluster worker, so never open such a resource before
 * app_listen. Validate and migrate in main(), close it, and open the real one in a hook:
 * the hook runs once in each serving process, after any fork.
 *   app/db.c (db_open / db_worker_init) is the full SQLite version of this pattern.
 * ------------------------------------------------------------------------------------------- */
static int g_worker_resource_opened;
static void open_resource_for_this_process(void) {
    g_worker_resource_opened = 1; /* e.g. sqlite3_open(...) into a static handle */
}

/* ---------------------------------------------------------------------------------------------
 * Registration: one place that shows the whole wiring order.
 * ------------------------------------------------------------------------------------------- */
void cookbook_register(App *app) {
    app_on_worker_start(app, open_resource_for_this_process);

    /* App-wide middleware runs in registration order, for every request (including 404s). */
    app_use(app, mw_request_id);
    app_use(app, mw_record_status);
    app_use_prefix(app, "/admin", mw_require_token); /* only paths under /admin */
    app_use_error(app, error_handler_json);

    app_get(app, "/hello", recipe_hello);
    app_get(app, "/square/:n", recipe_square);
    app_get(app, "/greet", recipe_greet);
    app_post(app, "/notes", recipe_create_note);
    app_get(app, "/numbers", recipe_numbers);
    app_get(app, "/admin/stats", recipe_admin_stats);
    app_post_mw(app, "/items", recipe_create_item, (Middleware[]){ mw_require_token }, 1); /* per-route */

    Router notes;
    router_init(&notes);
    router_get(&notes, "/", recipe_list_notes);
    router_get(&notes, "/:id", recipe_get_note);
    app_mount(app, "/v1/notes", &notes);

    app_get(app, "/login", recipe_login);
    app_get(app, "/whoami", recipe_whoami);
    app_get(app, "/logout", recipe_logout);
    app_post(app, "/form", recipe_form);
    app_post(app, "/upload", recipe_upload);
    app_get(app, "/old", recipe_old_path);
    app_get(app, "/go", recipe_go);
    app_delete(app, "/things/:id", recipe_delete_thing);
    app_post(app, "/things", recipe_created);
    app_get(app, "/stream", recipe_stream);
}

/* Exposed for tests/test_cookbook.c only. */
int cookbook_last_status_seen(void) { return g_last_status_seen; }
int cookbook_worker_resource_opened(void) { return g_worker_resource_opened; }
void cookbook_run_worker_hooks(App *app) {
    for (int i = 0; i < app->worker_init_hook_count; i++) {
        app->worker_init_hooks[i]();
    }
}
