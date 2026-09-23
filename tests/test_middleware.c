#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_types.h"
#include "router.h"
#include "response.h"
#include "middleware.h"

#include "arena.h"

static Connection *make_conn(void) {
    Connection *conn = calloc(1, sizeof(Connection));
    /* Connection.arena is now a pointer to a shared per-worker Arena (App.arena in the real
     * engine) rather than one embedded per connection - this fixture mallocs its own standalone
     * Arena to point at, same as it always mallocked its own 64 KiB buffer. */
    Arena *arena = malloc(sizeof(Arena));
    arena_init(arena, malloc(64 * 1024), 64 * 1024);
    conn->arena = arena;
    conn->keep_alive = 1;
    conn->file_fd = -1;
    return conn;
}

static void free_conn(Connection *conn) {
    void *buf = conn->arena->buf;
    arena_reset(conn->arena);
    free(buf);
    free(conn->arena);
    free(conn);
}

static void cleanup_app(App *app) {
    if (app != NULL) {
        free(app->connections);
        app->connections = NULL;
    }
}

static int g_order[8];
static int g_order_len;
static int g_handler_called;

static void record(int id) {
    g_order[g_order_len++] = id;
}

static void mw_a(const Request *req, Response *res, MiddlewareChain *chain) {
    (void)req;
    (void)res;
    record(1);
    chain_next(chain);
}

static void mw_b(const Request *req, Response *res, MiddlewareChain *chain) {
    (void)req;
    (void)res;
    record(2);
    chain_next(chain);
}

static void dummy_handler_unused(const Request *req, Response *res) {
    (void)req;
    (void)res;
}

static void handler_ok(const Request *req, Response *res) {
    (void)req;
    record(3);
    g_handler_called = 1;
    res_status(res, 200);
    res_send(res, "ok");
}

static void test_middlewares_run_in_order_then_handler(void) {
    g_order_len = 0;
    g_handler_called = 0;

    App app;
    app_init(&app);
    app_use(&app, mw_a);
    app_use(&app, mw_b);

    Route route = { "GET", "/", handler_ok, { 0 }, 0, NULL, 0 };
    Request req;
    memset(&req, 0, sizeof(req));
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    dispatch(&app, &route, &req, &res);

    assert(g_handler_called == 1);
    assert(g_order_len == 3);
    assert(g_order[0] == 1 && g_order[1] == 2 && g_order[2] == 3);
    assert(res.status == 200);
    assert(strstr(conn->out_buf, "ok") != NULL);

    free_conn(conn);
    cleanup_app(&app);
}

static void mw_short_circuit(const Request *req, Response *res, MiddlewareChain *chain) {
    (void)req;
    (void)chain;
    res_status(res, 403);
    res_send(res, "forbidden");
}

static void test_middleware_can_short_circuit(void) {
    g_handler_called = 0;

    App app;
    app_init(&app);
    app_use(&app, mw_short_circuit);

    Route route = { "GET", "/", handler_ok, { 0 }, 0, NULL, 0 };
    Request req;
    memset(&req, 0, sizeof(req));
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    dispatch(&app, &route, &req, &res);

    assert(g_handler_called == 0);
    assert(res.status == 403);
    assert(strstr(conn->out_buf, "forbidden") != NULL);

    free_conn(conn);
    cleanup_app(&app);
}

static void mw_passthrough(const Request *req, Response *res, MiddlewareChain *chain) {
    (void)req;
    (void)res;
    chain_next(chain);
}

static void test_dispatch_falls_through_to_404_without_route(void) {
    App app;
    app_init(&app);
    app_use(&app, mw_passthrough);

    Request req;
    memset(&req, 0, sizeof(req));
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    dispatch(&app, NULL, &req, &res);

    assert(res.status == 404);
    assert(strstr(conn->out_buf, "Not Found") != NULL);

    free_conn(conn);
    cleanup_app(&app);
}

static void test_dispatch_returns_405_when_path_matches_other_method(void) {
    App app;
    app_init(&app);
    app_use(&app, mw_passthrough);
    app_get(&app, "/users", dummy_handler_unused);
    app_post(&app, "/users", dummy_handler_unused);

    Request req;
    memset(&req, 0, sizeof(req));
    strncpy(req.method, "DELETE", sizeof(req.method) - 1);
    strncpy(req.path, "/users", sizeof(req.path) - 1);
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    /* Mirrors handle_readable: match_route finds nothing for DELETE, so
     * route is NULL going into dispatch even though the path is registered
     * under other methods. */
    dispatch(&app, NULL, &req, &res);

    assert(res.status == 405);
    assert(strstr(conn->out_buf, "Method Not Allowed") != NULL);
    assert(strstr(conn->out_buf, "Allow: GET, POST") != NULL);

    free_conn(conn);
    cleanup_app(&app);
}

static void test_dispatch_returns_404_when_path_matches_nothing(void) {
    App app;
    app_init(&app);
    app_get(&app, "/users", dummy_handler_unused);

    Request req;
    memset(&req, 0, sizeof(req));
    strncpy(req.method, "GET", sizeof(req.method) - 1);
    strncpy(req.path, "/notfound", sizeof(req.path) - 1);
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    dispatch(&app, NULL, &req, &res);

    assert(res.status == 404);
    assert(strstr(conn->out_buf, "Not Found") != NULL);
    assert(strstr(conn->out_buf, "Allow:") == NULL);

    free_conn(conn);
    cleanup_app(&app);
}

static int g_error_status;
static char g_error_message[128];

static void error_handler_capture(int status, const char *message, const Request *req, Response *res) {
    (void)req;
    g_error_status = status;
    strncpy(g_error_message, message, sizeof(g_error_message) - 1);
    g_error_message[sizeof(g_error_message) - 1] = '\0';
    res_status(res, status);
    res_send(res, message);
}

static void mw_fails(const Request *req, Response *res, MiddlewareChain *chain) {
    (void)req;
    (void)res;
    chain_error(chain, 400, "bad input");
}

static void test_chain_error_invokes_registered_error_handler(void) {
    g_error_status = 0;
    g_error_message[0] = '\0';

    App app;
    app_init(&app);
    app_use(&app, mw_fails);
    app_use_error(&app, error_handler_capture);

    Route route = { "GET", "/", handler_ok, { 0 }, 0, NULL, 0 };
    Request req;
    memset(&req, 0, sizeof(req));
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    dispatch(&app, &route, &req, &res);

    assert(g_error_status == 400);
    assert(strcmp(g_error_message, "bad input") == 0);
    assert(res.status == 400);

    free_conn(conn);
    cleanup_app(&app);
}

static void test_chain_error_default_fallback_without_handler(void) {
    App app;
    app_init(&app);
    app_use(&app, mw_fails);

    Route route = { "GET", "/", handler_ok, { 0 }, 0, NULL, 0 };
    Request req;
    memset(&req, 0, sizeof(req));
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    dispatch(&app, &route, &req, &res);

    assert(res.status == 400);
    assert(strstr(conn->out_buf, "bad input") != NULL);

    free_conn(conn);
    cleanup_app(&app);
}

static void mw_c(const Request *req, Response *res, MiddlewareChain *chain) {
    (void)req;
    (void)res;
    record(4);
    chain_next(chain);
}

static void test_route_middleware_runs_after_app_wide_before_handler(void) {
    g_order_len = 0;
    g_handler_called = 0;

    App app;
    app_init(&app);
    app_use(&app, mw_a);

    Middleware route_mw[] = { mw_b, mw_c };
    Route route = { "GET", "/", handler_ok, { 0 }, 0, NULL, 0 };
    route.middlewares[0] = route_mw[0];
    route.middlewares[1] = route_mw[1];
    route.middleware_count = 2;

    Request req;
    memset(&req, 0, sizeof(req));
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    dispatch(&app, &route, &req, &res);

    assert(g_handler_called == 1);
    assert(g_order_len == 4);
    assert(g_order[0] == 1 && g_order[1] == 2 && g_order[2] == 4 && g_order[3] == 3);
    assert(res.status == 200);

    free_conn(conn);
    cleanup_app(&app);
}

static void test_route_without_middleware_only_runs_app_wide(void) {
    g_order_len = 0;
    g_handler_called = 0;

    App app;
    app_init(&app);
    app_use(&app, mw_a);

    Route route = { "GET", "/", handler_ok, { 0 }, 0, NULL, 0 };
    Request req;
    memset(&req, 0, sizeof(req));
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    dispatch(&app, &route, &req, &res);

    assert(g_handler_called == 1);
    assert(g_order_len == 2);
    assert(g_order[0] == 1 && g_order[1] == 3);

    free_conn(conn);
    cleanup_app(&app);
}

static void mw_route_short_circuit(const Request *req, Response *res, MiddlewareChain *chain) {
    (void)req;
    (void)chain;
    res_status(res, 401);
    res_send(res, "route unauthorized");
}

static void test_route_middleware_can_short_circuit_before_handler(void) {
    g_order_len = 0;
    g_handler_called = 0;

    App app;
    app_init(&app);
    app_use(&app, mw_a);

    Route route = { "GET", "/", handler_ok, { 0 }, 0, NULL, 0 };
    route.middlewares[0] = mw_route_short_circuit;
    route.middleware_count = 1;

    Request req;
    memset(&req, 0, sizeof(req));
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    dispatch(&app, &route, &req, &res);

    assert(g_handler_called == 0);
    assert(g_order_len == 1 && g_order[0] == 1);
    assert(res.status == 401);
    assert(strstr(conn->out_buf, "route unauthorized") != NULL);

    free_conn(conn);
    cleanup_app(&app);
}

static void mw_prefixed(const Request *req, Response *res, MiddlewareChain *chain) {
    (void)req;
    (void)res;
    record(5);
    chain_next(chain);
}

static void test_prefixed_middleware_runs_when_path_matches(void) {
    g_order_len = 0;
    g_handler_called = 0;

    App app;
    app_init(&app);
    app_use_prefix(&app, "/api", mw_prefixed);
    app_use(&app, mw_a);

    Route route = { "GET", "/api/users", handler_ok, { 0 }, 0, NULL, 0 };
    Request req;
    memset(&req, 0, sizeof(req));
    strncpy(req.path, "/api/users", sizeof(req.path) - 1);
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    dispatch(&app, &route, &req, &res);

    assert(g_handler_called == 1);
    assert(g_order_len == 3);
    assert(g_order[0] == 5 && g_order[1] == 1 && g_order[2] == 3);
    assert(res.status == 200);

    free_conn(conn);
    cleanup_app(&app);
}

static void test_prefixed_middleware_skipped_when_path_does_not_match(void) {
    g_order_len = 0;
    g_handler_called = 0;

    App app;
    app_init(&app);
    app_use_prefix(&app, "/api", mw_prefixed);
    app_use(&app, mw_a);

    Route route = { "GET", "/other", handler_ok, { 0 }, 0, NULL, 0 };
    Request req;
    memset(&req, 0, sizeof(req));
    strncpy(req.path, "/other", sizeof(req.path) - 1);
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    dispatch(&app, &route, &req, &res);

    assert(g_handler_called == 1);
    assert(g_order_len == 2);
    assert(g_order[0] == 1 && g_order[1] == 3);
    assert(res.status == 200);

    free_conn(conn);
    cleanup_app(&app);
}

static void test_prefixed_middleware_does_not_match_similar_sibling_path(void) {
    g_order_len = 0;
    g_handler_called = 0;

    App app;
    app_init(&app);
    app_use_prefix(&app, "/api", mw_prefixed);

    Route route = { "GET", "/apiary", handler_ok, { 0 }, 0, NULL, 0 };
    Request req;
    memset(&req, 0, sizeof(req));
    strncpy(req.path, "/apiary", sizeof(req.path) - 1);
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    dispatch(&app, &route, &req, &res);

    assert(g_handler_called == 1);
    assert(g_order_len == 1);
    assert(g_order[0] == 3);

    free_conn(conn);
    cleanup_app(&app);
}

static void test_dispatch_auto_options_lists_allowed_methods(void) {
    App app;
    app_init(&app);
    app_get(&app, "/users", dummy_handler_unused);
    app_post(&app, "/users", dummy_handler_unused);

    Request req;
    memset(&req, 0, sizeof(req));
    strncpy(req.method, "OPTIONS", sizeof(req.method) - 1);
    req.method[sizeof(req.method) - 1] = '\0';
    strncpy(req.path, "/users", sizeof(req.path) - 1);
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    /* Mirrors handle_readable: no explicit OPTIONS route, so match_route
     * returns NULL even though /users matches under GET/POST. */
    dispatch(&app, NULL, &req, &res);

    assert(res.status == 200);
    /* GET implies HEAD via match_route's auto fallback, and OPTIONS itself
     * is always implicitly supported once this branch runs - both get
     * folded into the Allow header alongside the explicitly registered
     * methods. */
    assert(strstr(conn->out_buf, "Allow: GET, POST, HEAD, OPTIONS") != NULL);
    assert(strstr(conn->out_buf, "Content-Length: 0") != NULL);

    free_conn(conn);
    cleanup_app(&app);
}

static void test_dispatch_auto_options_without_get_omits_head(void) {
    App app;
    app_init(&app);
    app_post(&app, "/echo", dummy_handler_unused);

    Request req;
    memset(&req, 0, sizeof(req));
    strncpy(req.method, "OPTIONS", sizeof(req.method) - 1);
    req.method[sizeof(req.method) - 1] = '\0';
    strncpy(req.path, "/echo", sizeof(req.path) - 1);
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    dispatch(&app, NULL, &req, &res);

    assert(res.status == 200);
    assert(strstr(conn->out_buf, "Allow: POST, OPTIONS") != NULL);
    assert(strstr(conn->out_buf, "HEAD") == NULL);

    free_conn(conn);
    cleanup_app(&app);
}

static void test_dispatch_options_still_404s_unknown_path(void) {
    App app;
    app_init(&app);
    app_get(&app, "/users", dummy_handler_unused);

    Request req;
    memset(&req, 0, sizeof(req));
    strncpy(req.method, "OPTIONS", sizeof(req.method) - 1);
    req.method[sizeof(req.method) - 1] = '\0';
    strncpy(req.path, "/notfound", sizeof(req.path) - 1);
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    dispatch(&app, NULL, &req, &res);

    assert(res.status == 404);

    free_conn(conn);
    cleanup_app(&app);
}

static void test_dispatch_explicit_options_route_wins_over_auto(void) {
    g_handler_called = 0;

    App app;
    app_init(&app);
    app_get(&app, "/users", dummy_handler_unused);

    Route route = { "OPTIONS", "/users", handler_ok, { 0 }, 0, NULL, 0 };
    Request req;
    memset(&req, 0, sizeof(req));
    strncpy(req.method, "OPTIONS", sizeof(req.method) - 1);
    req.method[sizeof(req.method) - 1] = '\0';
    strncpy(req.path, "/users", sizeof(req.path) - 1);
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    /* Mirrors handle_readable: match_route finds the explicit OPTIONS route
     * directly, so dispatch is called with a non-NULL route - the auto
     * fallback (route == NULL) never comes into play. */
    dispatch(&app, &route, &req, &res);

    assert(g_handler_called == 1);
    assert(strstr(conn->out_buf, "ok") != NULL);

    free_conn(conn);
    cleanup_app(&app);
}

static void test_prefixed_middleware_prefix_is_normalized(void) {
    /* "/api/" used to match no sub-path at all (the byte after it is never '/' or NUL). */
    const char *prefixes[] = { "/api/", "api", "//api//" };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        g_order_len = 0;
        g_handler_called = 0;

        App app;
        app_init(&app);
        app_use_prefix(&app, prefixes[i], mw_prefixed);
        assert(strcmp(app.middlewares[0].prefix, "/api") == 0);

        Route route = { "GET", "/api/users", handler_ok, { 0 }, 0, NULL, 0 };
        Request req;
        memset(&req, 0, sizeof(req));
        strncpy(req.path, "/api/users", sizeof(req.path) - 1);
        Connection *conn = make_conn();
        Response res = { .conn = conn, .status = 0 };

        dispatch(&app, &route, &req, &res);

        assert(g_order_len == 2);
        assert(g_order[0] == 5 && g_order[1] == 3);

        free_conn(conn);
        cleanup_app(&app);
    }
}

int main(void) {
    test_middlewares_run_in_order_then_handler();
    test_middleware_can_short_circuit();
    test_dispatch_falls_through_to_404_without_route();
    test_dispatch_returns_405_when_path_matches_other_method();
    test_dispatch_returns_404_when_path_matches_nothing();
    test_chain_error_invokes_registered_error_handler();
    test_chain_error_default_fallback_without_handler();
    test_route_middleware_runs_after_app_wide_before_handler();
    test_route_without_middleware_only_runs_app_wide();
    test_route_middleware_can_short_circuit_before_handler();
    test_prefixed_middleware_runs_when_path_matches();
    test_prefixed_middleware_skipped_when_path_does_not_match();
    test_prefixed_middleware_does_not_match_similar_sibling_path();
    test_prefixed_middleware_prefix_is_normalized();
    test_dispatch_auto_options_lists_allowed_methods();
    test_dispatch_auto_options_without_get_omits_head();
    test_dispatch_options_still_404s_unknown_path();
    test_dispatch_explicit_options_route_wins_over_auto();

    printf("All middleware tests passed.\n");
    return 0;
}
