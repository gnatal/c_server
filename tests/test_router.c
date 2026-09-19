#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_types.h"
#include "router.h"

static void dummy_handler_a(const Request *req, Response *res) {
    (void)req;
    (void)res;
}

static void dummy_handler_b(const Request *req, Response *res) {
    (void)req;
    (void)res;
}

static void dummy_mw_a(const Request *req, Response *res, MiddlewareChain *chain) {
    (void)req;
    (void)res;
    (void)chain;
}

static void dummy_mw_b(const Request *req, Response *res, MiddlewareChain *chain) {
    (void)req;
    (void)res;
    (void)chain;
}

static void cleanup_app(App *app) {
    if (app != NULL) {
        free(app->connections);
        app->connections = NULL;
    }
}

static void test_match_path_exact_literals(void) {
    Request req;
    memset(&req, 0, sizeof(req));

    assert(match_path("/", "/", &req) == 1);
    assert(match_path("/users", "/users", &req) == 1);
    assert(match_path("/api/v1/status", "/api/v1/status", &req) == 1);

    /* Segment value mismatch */
    assert(match_path("/users", "/posts", &req) == 0);
    assert(match_path("/api/v1", "/api/v2", &req) == 0);

    /* Segment count / length mismatch */
    assert(match_path("/users", "/users/123", &req) == 0);
    assert(match_path("/users/123", "/users", &req) == 0);
    assert(match_path("/", "/users", &req) == 0);
    assert(match_path("/users", "/", &req) == 0);
}

static void test_match_path_params(void) {
    Request req;
    memset(&req, 0, sizeof(req));

    assert(match_path("/users/:id", "/users/42", &req) == 1);
    assert(req.param_count == 1);
    const char *id_val = req_get_param(&req, "id");
    assert(id_val != NULL && strcmp(id_val, "42") == 0);

    /* Multiple parameters */
    memset(&req, 0, sizeof(req));
    assert(match_path("/orgs/:orgId/repos/:repoName", "/orgs/google/repos/CExpress", &req) == 1);
    assert(req.param_count == 2);
    const char *org_val = req_get_param(&req, "orgId");
    const char *repo_val = req_get_param(&req, "repoName");
    assert(org_val != NULL && strcmp(org_val, "google") == 0);
    assert(repo_val != NULL && strcmp(repo_val, "CExpress") == 0);

    /* Non-existent param returns NULL */
    assert(req_get_param(&req, "nonexistent") == NULL);

    /* Path mismatch with param segment */
    memset(&req, 0, sizeof(req));
    assert(match_path("/users/:id/edit", "/users/42/view", &req) == 0);
}

static void test_match_path_max_params(void) {
    Request req;
    memset(&req, 0, sizeof(req));

    /* 9 parameter segments (MAX_PARAMS is 8) */
    const char *pattern = "/:p1/:p2/:p3/:p4/:p5/:p6/:p7/:p8/:p9";
    const char *path = "/1/2/3/4/5/6/7/8/9";

    assert(match_path(pattern, path, &req) == 1);
    assert(req.param_count == MAX_PARAMS);
    assert(strcmp(req_get_param(&req, "p1"), "1") == 0);
    assert(strcmp(req_get_param(&req, "p8"), "8") == 0);
    /* 9th param was safely not stored due to MAX_PARAMS limit */
    assert(req_get_param(&req, "p9") == NULL);
}

static void test_match_path_wildcards(void) {
    Request req;

    /* Trailing wildcard matches one or many remaining segments. */
    memset(&req, 0, sizeof(req));
    assert(match_path("/files/*", "/files/a", &req) == 1);
    memset(&req, 0, sizeof(req));
    assert(match_path("/files/*", "/files/a/b/c", &req) == 1);

    /* But not the prefix alone - there's no trailing segment to match "*" against. */
    memset(&req, 0, sizeof(req));
    assert(match_path("/files/*", "/files", &req) == 0);

    /* Sibling paths that merely share a prefix don't match. */
    memset(&req, 0, sizeof(req));
    assert(match_path("/files/*", "/other/a", &req) == 0);

    /* Mid-path wildcard matches exactly one segment, not capturing a param. */
    memset(&req, 0, sizeof(req));
    assert(match_path("/users/*/edit", "/users/42/edit", &req) == 1);
    assert(req.param_count == 0);
    memset(&req, 0, sizeof(req));
    assert(match_path("/users/*/edit", "/users/42/43/edit", &req) == 0);
    memset(&req, 0, sizeof(req));
    assert(match_path("/users/*/edit", "/users/42/view", &req) == 0);

    /* Wildcard and named params can combine on the same pattern. */
    memset(&req, 0, sizeof(req));
    assert(match_path("/users/:id/*", "/users/42/anything/nested", &req) == 1);
    assert(strcmp(req_get_param(&req, "id"), "42") == 0);
}

static void test_match_route(void) {
    App app;
    app_init(&app);

    app_get(&app, "/users", dummy_handler_a);
    app_get(&app, "/users/:id", dummy_handler_b);
    app_post(&app, "/users", dummy_handler_b);

    Request req;
    memset(&req, 0, sizeof(req));
    strncpy(req.method, "GET", sizeof(req.method) - 1);
    strncpy(req.path, "/users", sizeof(req.path) - 1);

    const Route *matched = match_route(&app, &req);
    assert(matched != NULL);
    assert(matched->handler == dummy_handler_a);
    assert(strcmp(matched->path, "/users") == 0);

    /* Method matching: POST to /users */
    memset(&req, 0, sizeof(req));
    strncpy(req.method, "POST", sizeof(req.method) - 1);
    strncpy(req.path, "/users", sizeof(req.path) - 1);
    matched = match_route(&app, &req);
    assert(matched != NULL);
    assert(matched->handler == dummy_handler_b);

    /* Parameterized route matching */
    memset(&req, 0, sizeof(req));
    strncpy(req.method, "GET", sizeof(req.method) - 1);
    strncpy(req.path, "/users/100", sizeof(req.path) - 1);
    matched = match_route(&app, &req);
    assert(matched != NULL);
    assert(matched->handler == dummy_handler_b);
    assert(strcmp(req_get_param(&req, "id"), "100") == 0);

    /* Unmatched route returns NULL */
    memset(&req, 0, sizeof(req));
    strncpy(req.method, "DELETE", sizeof(req.method) - 1);
    strncpy(req.path, "/users", sizeof(req.path) - 1);
    assert(match_route(&app, &req) == NULL);

    memset(&req, 0, sizeof(req));
    strncpy(req.method, "GET", sizeof(req.method) - 1);
    strncpy(req.path, "/notfound", sizeof(req.path) - 1);
    assert(match_route(&app, &req) == NULL);
    cleanup_app(&app);
}

static void test_match_route_allowed_methods(void) {
    App app;
    app_init(&app);

    app_get(&app, "/users", dummy_handler_a);
    app_post(&app, "/users", dummy_handler_b);
    app_get(&app, "/users/:id", dummy_handler_b);

    char allowed[64];

    /* Path matches two routes under different methods - both listed, in
     * registration order, deduplicated. */
    Request req;
    memset(&req, 0, sizeof(req));
    strncpy(req.method, "DELETE", sizeof(req.method) - 1);
    strncpy(req.path, "/users", sizeof(req.path) - 1);
    int count = match_route_allowed_methods(&app, &req, allowed, sizeof(allowed));
    assert(count == 2);
    assert(strcmp(allowed, "GET, POST") == 0);
    /* req itself is untouched (no stray param_count side effect). */
    assert(req.param_count == 0);

    /* Path matches exactly one route under a different method. */
    memset(&req, 0, sizeof(req));
    strncpy(req.method, "DELETE", sizeof(req.method) - 1);
    strncpy(req.path, "/users/42", sizeof(req.path) - 1);
    count = match_route_allowed_methods(&app, &req, allowed, sizeof(allowed));
    assert(count == 1);
    assert(strcmp(allowed, "GET") == 0);

    /* Completely unknown path matches nothing - a real 404. */
    memset(&req, 0, sizeof(req));
    strncpy(req.method, "GET", sizeof(req.method) - 1);
    strncpy(req.path, "/notfound", sizeof(req.path) - 1);
    count = match_route_allowed_methods(&app, &req, allowed, sizeof(allowed));
    assert(count == 0);
    assert(allowed[0] == '\0');
    cleanup_app(&app);
}

static void test_app_add_route_overflow(void) {
    App app;
    app_init(&app);
    assert(app.route_count == 0);

    for (int i = 0; i < MAX_ROUTES; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/route%d", i);
        app_get(&app, path, dummy_handler_a);
    }
    assert(app.route_count == MAX_ROUTES);

    /* Exceeding MAX_ROUTES should be rejected safely */
    app_get(&app, "/overflow", dummy_handler_a);
    assert(app.route_count == MAX_ROUTES);
    cleanup_app(&app);
}

static void test_app_get_mw_stores_route_middleware(void) {
    App app;
    app_init(&app);

    Middleware mws[] = { dummy_mw_a, dummy_mw_b };
    app_get_mw(&app, "/protected", dummy_handler_a, mws, 2);

    assert(app.route_count == 1);
    const Route *route = &app.routes[0];
    assert(route->middleware_count == 2);
    assert(route->middlewares[0] == dummy_mw_a);
    assert(route->middlewares[1] == dummy_mw_b);

    /* app_get (no middleware) still yields a route with zero middleware. */
    app_get(&app, "/open", dummy_handler_a);
    assert(app.routes[1].middleware_count == 0);
    cleanup_app(&app);
}

static void test_app_post_mw_stores_route_middleware(void) {
    App app;
    app_init(&app);

    Middleware mws[] = { dummy_mw_a };
    app_post_mw(&app, "/protected", dummy_handler_b, mws, 1);

    assert(app.route_count == 1);
    assert(app.routes[0].middleware_count == 1);
    assert(app.routes[0].middlewares[0] == dummy_mw_a);
    cleanup_app(&app);
}

static void test_app_add_route_mw_truncates_overflow(void) {
    App app;
    app_init(&app);

    Middleware mws[MAX_ROUTE_MIDDLEWARES + 2];
    for (int i = 0; i < MAX_ROUTE_MIDDLEWARES + 2; i++) {
        mws[i] = dummy_mw_a;
    }

    app_add_route_mw(&app, "GET", "/many", dummy_handler_a, mws, MAX_ROUTE_MIDDLEWARES + 2);

    assert(app.route_count == 1);
    assert(app.routes[0].middleware_count == MAX_ROUTE_MIDDLEWARES);
    cleanup_app(&app);
}

static void test_app_put_patch_delete_register_correct_methods(void) {
    App app;
    app_init(&app);

    app_put(&app, "/users/:id", dummy_handler_a);
    app_patch(&app, "/users/:id", dummy_handler_a);
    app_delete(&app, "/users/:id", dummy_handler_a);

    assert(app.route_count == 3);
    assert(strcmp(app.routes[0].method, "PUT") == 0);
    assert(strcmp(app.routes[1].method, "PATCH") == 0);
    assert(strcmp(app.routes[2].method, "DELETE") == 0);

    Request req;
    memset(&req, 0, sizeof(req));
    strncpy(req.method, "DELETE", sizeof(req.method) - 1);
    strncpy(req.path, "/users/7", sizeof(req.path) - 1);
    const Route *matched = match_route(&app, &req);
    assert(matched != NULL && matched->handler == dummy_handler_a);
    assert(strcmp(req_get_param(&req, "id"), "7") == 0);

    /* PUT to a DELETE-only path still 404s - method matters, not just path. */
    memset(&req, 0, sizeof(req));
    strncpy(req.method, "PUT", sizeof(req.method) - 1);
    strncpy(req.path, "/users/7", sizeof(req.path) - 1);
    matched = match_route(&app, &req);
    assert(matched != NULL && strcmp(matched->method, "PUT") == 0);
    cleanup_app(&app);
}

static void test_app_put_patch_delete_mw_store_route_middleware(void) {
    App app;
    app_init(&app);

    Middleware mws[] = { dummy_mw_a };
    app_put_mw(&app, "/users/:id", dummy_handler_a, mws, 1);
    app_patch_mw(&app, "/users/:id", dummy_handler_a, mws, 1);
    app_delete_mw(&app, "/users/:id", dummy_handler_a, mws, 1);

    assert(app.route_count == 3);
    for (int i = 0; i < 3; i++) {
        assert(app.routes[i].middleware_count == 1);
        assert(app.routes[i].middlewares[0] == dummy_mw_a);
    }
    cleanup_app(&app);
}

static void test_router_put_patch_delete_register_correct_methods(void) {
    Router router;
    router_init(&router);

    router_put(&router, "/users/:id", dummy_handler_a);
    router_patch(&router, "/users/:id", dummy_handler_a);
    router_delete(&router, "/users/:id", dummy_handler_a);

    Middleware mws[] = { dummy_mw_b };
    router_put_mw(&router, "/protected", dummy_handler_b, mws, 1);
    router_patch_mw(&router, "/protected", dummy_handler_b, mws, 1);
    router_delete_mw(&router, "/protected", dummy_handler_b, mws, 1);

    assert(router.route_count == 6);
    assert(strcmp(router.routes[0].method, "PUT") == 0);
    assert(strcmp(router.routes[1].method, "PATCH") == 0);
    assert(strcmp(router.routes[2].method, "DELETE") == 0);
    assert(strcmp(router.routes[3].method, "PUT") == 0 && router.routes[3].middleware_count == 1);
    assert(strcmp(router.routes[4].method, "PATCH") == 0 && router.routes[4].middleware_count == 1);
    assert(strcmp(router.routes[5].method, "DELETE") == 0 && router.routes[5].middleware_count == 1);

    App app;
    app_init(&app);
    app_mount(&app, "/api", &router);
    assert(app.route_count == 6);
    assert(strcmp(app.routes[0].path, "/api/users/:id") == 0);
    cleanup_app(&app);
}

static void test_app_mount_prefixes_router_routes(void) {
    App app;
    app_init(&app);

    Router router;
    router_init(&router);
    router_get(&router, "/users", dummy_handler_a);
    router_get(&router, "/", dummy_handler_b);

    app_mount(&app, "/api", &router);

    assert(app.route_count == 2);
    assert(strcmp(app.routes[0].path, "/api/users") == 0);
    assert(app.routes[0].handler == dummy_handler_a);
    /* A router route registered at "/" mounts at the prefix itself, not
     * "/api/" - so it matches GET /api, not GET /api/. */
    assert(strcmp(app.routes[1].path, "/api") == 0);
    assert(app.routes[1].handler == dummy_handler_b);

    Request req;
    memset(&req, 0, sizeof(req));
    strncpy(req.method, "GET", sizeof(req.method) - 1);
    strncpy(req.path, "/api/users", sizeof(req.path) - 1);
    const Route *matched = match_route(&app, &req);
    assert(matched != NULL && matched->handler == dummy_handler_a);

    memset(&req, 0, sizeof(req));
    strncpy(req.method, "GET", sizeof(req.method) - 1);
    strncpy(req.path, "/users", sizeof(req.path) - 1);
    assert(match_route(&app, &req) == NULL);
    cleanup_app(&app);
}

static void test_app_mount_carries_route_middleware(void) {
    App app;
    app_init(&app);

    Router router;
    router_init(&router);
    Middleware mws[] = { dummy_mw_a };
    router_get_mw(&router, "/protected", dummy_handler_a, mws, 1);

    app_mount(&app, "/api", &router);

    assert(app.route_count == 1);
    assert(strcmp(app.routes[0].path, "/api/protected") == 0);
    assert(app.routes[0].middleware_count == 1);
    assert(app.routes[0].middlewares[0] == dummy_mw_a);
    cleanup_app(&app);
}

static void test_app_mount_scopes_router_middleware_to_prefix(void) {
    App app;
    app_init(&app);

    Router router;
    router_init(&router);
    router_use(&router, dummy_mw_a);

    app_mount(&app, "/api", &router);

    assert(app.middleware_count == 1);
    assert(app.middlewares[0].fn == dummy_mw_a);
    assert(strcmp(app.middlewares[0].prefix, "/api") == 0);
    cleanup_app(&app);
}

static void test_app_mount_strips_trailing_slash_from_prefix(void) {
    App app;
    app_init(&app);

    Router router;
    router_init(&router);
    router_get(&router, "/users", dummy_handler_a);

    app_mount(&app, "/api/", &router);

    assert(app.route_count == 1);
    assert(strcmp(app.routes[0].path, "/api/users") == 0);
    cleanup_app(&app);
}

static void test_app_mount_root_prefix_is_unscoped(void) {
    App app;
    app_init(&app);

    Router router;
    router_init(&router);
    router_get(&router, "/users", dummy_handler_a);
    router_use(&router, dummy_mw_a);

    app_mount(&app, "/", &router);

    assert(app.route_count == 1);
    assert(strcmp(app.routes[0].path, "/users") == 0);
    assert(app.middleware_count == 1);
    assert(app.middlewares[0].prefix[0] == '\0');
    cleanup_app(&app);
}

static void test_app_mount_respects_max_routes(void) {
    App app;
    app_init(&app);

    for (int i = 0; i < MAX_ROUTES; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/route%d", i);
        app_get(&app, path, dummy_handler_a);
    }
    assert(app.route_count == MAX_ROUTES);

    Router router;
    router_init(&router);
    router_get(&router, "/overflow", dummy_handler_b);

    app_mount(&app, "/api", &router);
    assert(app.route_count == MAX_ROUTES);
    cleanup_app(&app);
}

static void test_app_head_options_register_correct_methods(void) {
    App app;
    app_init(&app);

    app_head(&app, "/users/:id", dummy_handler_a);
    app_options(&app, "/users/:id", dummy_handler_a);

    assert(app.route_count == 2);
    assert(strcmp(app.routes[0].method, "HEAD") == 0);
    assert(strcmp(app.routes[1].method, "OPTIONS") == 0);

    Middleware mws[] = { dummy_mw_a };
    app_head_mw(&app, "/protected", dummy_handler_b, mws, 1);
    app_options_mw(&app, "/protected", dummy_handler_b, mws, 1);
    assert(app.route_count == 4);
    assert(app.routes[2].middleware_count == 1 && app.routes[2].middlewares[0] == dummy_mw_a);
    assert(app.routes[3].middleware_count == 1 && app.routes[3].middlewares[0] == dummy_mw_a);
    cleanup_app(&app);
}

static void test_router_head_options_register_correct_methods(void) {
    Router router;
    router_init(&router);

    router_head(&router, "/users/:id", dummy_handler_a);
    router_options(&router, "/users/:id", dummy_handler_a);

    Middleware mws[] = { dummy_mw_b };
    router_head_mw(&router, "/protected", dummy_handler_b, mws, 1);
    router_options_mw(&router, "/protected", dummy_handler_b, mws, 1);

    assert(router.route_count == 4);
    assert(strcmp(router.routes[0].method, "HEAD") == 0);
    assert(strcmp(router.routes[1].method, "OPTIONS") == 0);
    assert(strcmp(router.routes[2].method, "HEAD") == 0 && router.routes[2].middleware_count == 1);
    assert(strcmp(router.routes[3].method, "OPTIONS") == 0 && router.routes[3].middleware_count == 1);
}

static void test_match_route_head_falls_back_to_get(void) {
    App app;
    app_init(&app);

    app_get(&app, "/users/:id", dummy_handler_a);

    Request req;
    memset(&req, 0, sizeof(req));
    strncpy(req.method, "HEAD", sizeof(req.method) - 1);
    strncpy(req.path, "/users/42", sizeof(req.path) - 1);

    const Route *matched = match_route(&app, &req);
    assert(matched != NULL);
    assert(matched->handler == dummy_handler_a);
    assert(strcmp(matched->method, "GET") == 0);
    assert(strcmp(req_get_param(&req, "id"), "42") == 0);

    /* A HEAD request to a path with no GET (or HEAD) route still misses. */
    memset(&req, 0, sizeof(req));
    strncpy(req.method, "HEAD", sizeof(req.method) - 1);
    strncpy(req.path, "/notfound", sizeof(req.path) - 1);
    assert(match_route(&app, &req) == NULL);
    cleanup_app(&app);
}

static void test_match_route_explicit_head_wins_over_get_fallback(void) {
    App app;
    app_init(&app);

    app_get(&app, "/users", dummy_handler_a);
    app_head(&app, "/users", dummy_handler_b);

    Request req;
    memset(&req, 0, sizeof(req));
    strncpy(req.method, "HEAD", sizeof(req.method) - 1);
    strncpy(req.path, "/users", sizeof(req.path) - 1);

    const Route *matched = match_route(&app, &req);
    assert(matched != NULL);
    /* The explicit HEAD route, not the GET fallback. */
    assert(matched->handler == dummy_handler_b);
    assert(strcmp(matched->method, "HEAD") == 0);
    cleanup_app(&app);
}

static void test_match_path_without_request_captures_nothing(void) {
    /* req == NULL means "just tell me if it matches" (used by match_route_allowed_methods). */
    assert(match_path("/users/:id", "/users/5", NULL) == 1);
    assert(match_path("/users/:id", "/users", NULL) == 0);
    assert(match_path("/files/*", "/files/a/b/c", NULL) == 1);
    assert(match_path("/a/*/c", "/a/b/c", NULL) == 1);
    assert(match_path("/a/*/c", "/a/b/x", NULL) == 0);
}

static void test_match_path_segments_and_truncation(void) {
    Request req;
    memset(&req, 0xA5, sizeof(req)); /* garbage: match_path must terminate everything it writes */

    /* Empty segments are ignored on both sides, like a tokenizer would. */
    assert(match_path("//users//:id/", "/users/9//", &req) == 1);
    assert(req.param_count == 1);
    assert(strcmp(req.param_names[0], "id") == 0);
    assert(strcmp(req_get_param(&req, "id"), "9") == 0);

    /* Root pattern matches only the root. */
    assert(match_path("/", "/", &req) == 1);
    assert(match_path("/", "", &req) == 1);
    assert(match_path("/", "/x", &req) == 0);
    assert(req.param_count == 0);

    /* A path segment longer than a param slot is truncated and NUL-terminated. */
    char long_path[200] = "/users/";
    memset(long_path + 7, 'z', 150);
    long_path[157] = '\0';
    assert(match_path("/users/:id", long_path, &req) == 1);
    assert(strlen(req.param_values[0]) == sizeof(req.param_values[0]) - 1);

    /* A literal must match whole segments, not prefixes. */
    assert(match_path("/api", "/apiary", &req) == 0);
    assert(match_path("/apiary", "/api", &req) == 0);
    assert(match_path("/a/b", "/a/bc", &req) == 0);

    /* A failed match after a capture leaves param_count for the next attempt to reset. */
    assert(match_path("/u/:id/x", "/u/1/y", &req) == 0);
    assert(match_path("/u/:id/y", "/u/1/y", &req) == 1);
    assert(req.param_count == 1);
}

static void test_allowed_methods_does_not_touch_request(void) {
    App app;
    app_init(&app);
    app_get(&app, "/users/:id", dummy_handler_a);
    app_put(&app, "/users/:id", dummy_handler_b);

    Request req;
    memset(&req, 0, sizeof(req));
    strncpy(req.method, "DELETE", sizeof(req.method) - 1);
    strncpy(req.path, "/users/5", sizeof(req.path) - 1);
    req.param_count = 0;

    char allowed[64];
    assert(match_route_allowed_methods(&app, &req, allowed, sizeof(allowed)) == 2);
    assert(strcmp(allowed, "GET, PUT") == 0);
    assert(req.param_count == 0); /* no scratch capture leaked into the caller's request */
    cleanup_app(&app); /* app_init's table; app_destroy lives in connection.c, not linked here */
}

int main(void) {
    test_match_path_without_request_captures_nothing();
    test_match_path_segments_and_truncation();
    test_allowed_methods_does_not_touch_request();
    test_match_path_exact_literals();
    test_match_path_params();
    test_match_path_max_params();
    test_match_path_wildcards();
    test_match_route();
    test_match_route_allowed_methods();
    test_app_add_route_overflow();
    test_app_get_mw_stores_route_middleware();
    test_app_post_mw_stores_route_middleware();
    test_app_add_route_mw_truncates_overflow();
    test_app_put_patch_delete_register_correct_methods();
    test_app_put_patch_delete_mw_store_route_middleware();
    test_router_put_patch_delete_register_correct_methods();
    test_app_mount_prefixes_router_routes();
    test_app_mount_carries_route_middleware();
    test_app_mount_scopes_router_middleware_to_prefix();
    test_app_mount_strips_trailing_slash_from_prefix();
    test_app_mount_root_prefix_is_unscoped();
    test_app_mount_respects_max_routes();
    test_app_head_options_register_correct_methods();
    test_router_head_options_register_correct_methods();
    test_match_route_head_falls_back_to_get();
    test_match_route_explicit_head_wins_over_get_fallback();

    printf("all router tests passed\n");
    return 0;
}
