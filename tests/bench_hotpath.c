/*
 * `make bench`: CPU cost per request of the pure path parse -> route -> dispatch -> response
 * build, and of emitting a 20-row JSON list, with no sockets. Not a test: prints numbers.
 * Compare before/after when touching lib/http_parser.c, router.c, middleware.c, response.c or
 * lib/arena.c. Numbers are single-core and machine-dependent; only ratios are meaningful.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cexpress.h"
#include "arena.h"
#include "vendor/yyjson/yyjson.h"

Arena test_arena;
char test_arena_buf[64 * 1024];


static void handler_ok(const Request *req, Response *res) { (void)req; res_json(res, "{\"ok\":true}"); }
static void mw_pass(const Request *req, Response *res, MiddlewareChain *chain) { (void)req; (void)res; chain_next(chain); }

/* P3: req_get_cookie/req_get_header (Connection is already read by request_wants_close on every
 * request regardless of handler, see one_request below) are lazy now - a handler that never calls
 * them never pays for the split/materialization. handler_ok above is the common case; this one is
 * the "handler actually reads a cookie and a header" case, run separately below so both ends of that
 * trade-off are on the record, not just the faster one. */
static void handler_reads_cookie_and_header(const Request *req, Response *res) {
    const char *session = req_get_cookie(req, "session");
    const char *ua = req_get_header(req, "User-Agent");
    (void)session; (void)ua;
    res_json(res, "{\"ok\":true}");
}

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static const char *MINIMAL_GET = "GET /api/todos/42 HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n\r\n";
static const char *BROWSER_GET =
    "GET /api/todos/42?done=true&q=hello%20world HTTP/1.1\r\n"
    "Host: localhost:8080\r\nConnection: keep-alive\r\n"
    "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Safari/537.36\r\n"
    "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8\r\n"
    "Accept-Encoding: gzip, deflate, br\r\nAccept-Language: en-US,en;q=0.9\r\n"
    "Cookie: session=abcdef0123456789; theme=dark; _ga=GA1.1.123456789.1700000000\r\n"
    "Sec-Fetch-Site: same-origin\r\nSec-Fetch-Mode: cors\r\nSec-Fetch-Dest: empty\r\n\r\n";
static const char *POST_JSON =
    "POST /api/todos HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\nContent-Length: 23\r\n\r\n{\"title\":\"buy the milk\"}";
static const char *MISS_404 = "GET /nope/nothing/here HTTP/1.1\r\nHost: x\r\n\r\n";
static const char *BROWSER_GET_COOKIE_ROUTE =
    "GET /cookie-check?done=true&q=hello%20world HTTP/1.1\r\n"
    "Host: localhost:8080\r\nConnection: keep-alive\r\n"
    "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Safari/537.36\r\n"
    "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8\r\n"
    "Accept-Encoding: gzip, deflate, br\r\nAccept-Language: en-US,en;q=0.9\r\n"
    "Cookie: session=abcdef0123456789; theme=dark; _ga=GA1.1.123456789.1700000000\r\n"
    "Sec-Fetch-Site: same-origin\r\nSec-Fetch-Mode: cors\r\nSec-Fetch-Dest: empty\r\n\r\n";

/* Same sequence as connection.c: handle_readable (P2: one parse_request_head pass, reused by the
 * completeness check and the full parse, instead of request_is_complete and parse_http_request each
 * running their own). */
static void one_request(const App *app_const, const char *raw, const size_t len, Connection *conn) {
    App *app = (App *)app_const;
    Request req;
    req.body = NULL;
    ParsedHead head;
    parse_request_head(raw, len, &head);
    if (request_head_is_complete(&head, raw, len, NULL) &&
        parse_http_request_from_head(raw, len, &head, &req, &test_arena) == 0) {
        Response res;
        res_init(&res, conn);
        conn->keep_alive = !request_wants_close(&req);
        dispatch(app, match_route(app, &req), &req, &res);
    }
    arena_reset(&test_arena);
    arena_reset(conn->arena);
}

static void run(const char *label, App *app, const char *raw, const int iterations) {
    Connection conn;
    memset(&conn, 0, sizeof(conn));
    /* M1: Connection.arena is a pointer to a shared per-worker Arena (App.arena in the real engine)
     * now, not one embedded per connection - point it at its own local Arena, same buffer as before. */
    Arena conn_arena;
    arena_init(&conn_arena, test_arena_buf, sizeof(test_arena_buf));
    conn.arena = &conn_arena;
    conn.file_fd = -1;
    const size_t len = strlen(raw);
    for (int i = 0; i < iterations / 10; i++) one_request(app, raw, len, &conn);
    const double start = now_ns();
    for (int i = 0; i < iterations; i++) one_request(app, raw, len, &conn);
    const double per_request = (now_ns() - start) / iterations;
    printf("%-46s %7.0f ns/request\n", label, per_request);
}

typedef struct { long long id; const char *title; int done; } Row;

static void emit_rows(const Row *rows, const int count) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    yyjson_mut_doc_set_root(doc, arr);
    
    for (int i = 0; i < count; i++) {
        yyjson_mut_val *obj = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_int(doc, obj, "id", rows[i].id);
        yyjson_mut_obj_add_str(doc, obj, "title", rows[i].title);
        yyjson_mut_obj_add_bool(doc, obj, "done", rows[i].done);
        yyjson_mut_arr_append(arr, obj);
    }
    
    char *out = yyjson_mut_write(doc, 0, NULL);
    free(out);
    yyjson_mut_doc_free(doc);
}



int main(void) {
    arena_init(&test_arena, test_arena_buf, sizeof(test_arena_buf));
    static App app;
    app_init(&app);
    app_use(&app, mw_pass);
    app_get(&app, "/", handler_ok);
    app_get(&app, "/health", handler_ok);
    Router todos;
    router_init(&todos);
    router_get(&todos, "/", handler_ok);
    router_get(&todos, "/:id", handler_ok);
    router_post(&todos, "/", handler_ok);
    router_put(&todos, "/:id", handler_ok);
    router_patch(&todos, "/:id", handler_ok);
    router_delete(&todos, "/:id", handler_ok);
    app_mount(&app, "/api/todos", &todos);
    app_get(&app, "/cookie-check", handler_reads_cookie_and_header);

    const int n = 500000;
    printf("request path (8 routes, 1 middleware):\n");
    run("  minimal GET (2 headers)", &app, MINIMAL_GET, n);
    run("  browser GET (10 headers, cookies, query)", &app, BROWSER_GET, n);
    run("  ...same, handler reads a cookie+header", &app, BROWSER_GET_COOKIE_ROUTE, n);
    run("  POST with JSON body", &app, POST_JSON, n);
    run("  404 (no route matches)", &app, MISS_404, n);

    Row rows[20];
    for (int i = 0; i < 20; i++) { rows[i].id = 1000000 + i; rows[i].title = "Todo number: buy milk"; rows[i].done = i & 1; }
    const int m = 50000;
    double start = now_ns();
    for (int i = 0; i < m; i++) emit_rows(rows, 20);
    const double writer_ns = (now_ns() - start) / m;
    printf("\nJSON, 20-row list:\n  yyjson %8.0f ns\n", writer_ns);

    printf("\nsizeof(Request)=%zu sizeof(Response)=%zu sizeof(Connection)=%zu\n",
           sizeof(Request), sizeof(Response), sizeof(Connection));
    app_destroy(&app);
    return 0;
}
