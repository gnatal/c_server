#include <assert.h>
#include <stdio.h>
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
}

int main(void) {
    test_match_path_exact_literals();
    test_match_path_params();
    test_match_path_max_params();
    test_match_route();
    test_app_add_route_overflow();

    printf("all router tests passed\n");
    return 0;
}
