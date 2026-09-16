#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_types.h"
#include "router.h"
#include "response.h"
#include "middleware.h"

static Connection *make_conn(void) {
    Connection *conn = calloc(1, sizeof(Connection));
    conn->keep_alive = 1;
    return conn;
}

static void free_conn(Connection *conn) {
    free(conn->out_buf);
    free(conn);
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

    Route route = { "GET", "/", handler_ok };
    Request req;
    memset(&req, 0, sizeof(req));
    Connection *conn = make_conn();
    Response res = { conn, 0 };

    dispatch(&app, &route, &req, &res);

    assert(g_handler_called == 1);
    assert(g_order_len == 3);
    assert(g_order[0] == 1 && g_order[1] == 2 && g_order[2] == 3);
    assert(res.status == 200);
    assert(strstr(conn->out_buf, "ok") != NULL);

    free_conn(conn);
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

    Route route = { "GET", "/", handler_ok };
    Request req;
    memset(&req, 0, sizeof(req));
    Connection *conn = make_conn();
    Response res = { conn, 0 };

    dispatch(&app, &route, &req, &res);

    assert(g_handler_called == 0);
    assert(res.status == 403);
    assert(strstr(conn->out_buf, "forbidden") != NULL);

    free_conn(conn);
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
    Response res = { conn, 0 };

    dispatch(&app, NULL, &req, &res);

    assert(res.status == 404);
    assert(strstr(conn->out_buf, "Not Found") != NULL);

    free_conn(conn);
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

    Route route = { "GET", "/", handler_ok };
    Request req;
    memset(&req, 0, sizeof(req));
    Connection *conn = make_conn();
    Response res = { conn, 0 };

    dispatch(&app, &route, &req, &res);

    assert(g_error_status == 400);
    assert(strcmp(g_error_message, "bad input") == 0);
    assert(res.status == 400);

    free_conn(conn);
}

static void test_chain_error_default_fallback_without_handler(void) {
    App app;
    app_init(&app);
    app_use(&app, mw_fails);

    Route route = { "GET", "/", handler_ok };
    Request req;
    memset(&req, 0, sizeof(req));
    Connection *conn = make_conn();
    Response res = { conn, 0 };

    dispatch(&app, &route, &req, &res);

    assert(res.status == 400);
    assert(strstr(conn->out_buf, "bad input") != NULL);

    free_conn(conn);
}

int main(void) {
    test_middlewares_run_in_order_then_handler();
    test_middleware_can_short_circuit();
    test_dispatch_falls_through_to_404_without_route();
    test_chain_error_invokes_registered_error_handler();
    test_chain_error_default_fallback_without_handler();

    printf("All middleware tests passed.\n");
    return 0;
}
