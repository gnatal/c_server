#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "app_types.h"
#include "static.h"
#include "response.h"

/* ---- static_resolve_relative_path (pure) ---- */

static void test_resolve_literal_subpath(void) {
    char out[256];
    assert(static_resolve_relative_path("/static/*", "/static/css/app.css", out, sizeof(out)) == 0);
    assert(strcmp(out, "css/app.css") == 0);
}

static void test_resolve_root_mount(void) {
    char out[256];
    assert(static_resolve_relative_path("/*", "/img/logo.png", out, sizeof(out)) == 0);
    assert(strcmp(out, "img/logo.png") == 0);
}

static void test_resolve_rejects_dotdot_at_start(void) {
    char out[256];
    assert(static_resolve_relative_path("/static/*", "/static/../secret", out, sizeof(out)) == -1);
}

static void test_resolve_rejects_dotdot_in_middle(void) {
    char out[256];
    assert(static_resolve_relative_path("/static/*", "/static/a/../../secret", out, sizeof(out)) == -1);
}

static void test_resolve_rejects_dotdot_at_end(void) {
    char out[256];
    assert(static_resolve_relative_path("/static/*", "/static/a/..", out, sizeof(out)) == -1);
}

static void test_resolve_rejects_path_outside_mount(void) {
    char out[256];
    assert(static_resolve_relative_path("/static/*", "/other/file.txt", out, sizeof(out)) == -1);
}

static void test_resolve_rejects_bare_mount_path(void) {
    char out[256];
    assert(static_resolve_relative_path("/static/*", "/static", out, sizeof(out)) == -1);
    assert(static_resolve_relative_path("/static/*", "/static/", out, sizeof(out)) == -1);
}

static void test_resolve_rejects_buffer_too_small(void) {
    char out[4];
    assert(static_resolve_relative_path("/static/*", "/static/css/app.css", out, sizeof(out)) == -1);
}

/* ---- static_mime_type (pure) ---- */

static void test_mime_type_known_extensions(void) {
    assert(strcmp(static_mime_type("/a/b.html"), "text/html") == 0);
    assert(strcmp(static_mime_type("/a/b.css"), "text/css") == 0);
    assert(strcmp(static_mime_type("/a/b.CSS"), "text/css") == 0);
    assert(strcmp(static_mime_type("/a/b.json"), "application/json") == 0);
}

static void test_mime_type_defaults_to_octet_stream(void) {
    assert(strcmp(static_mime_type("/a/b.xyz"), "application/octet-stream") == 0);
    assert(strcmp(static_mime_type("/a/b"), "application/octet-stream") == 0);
}

/* ---- static_serve_file (I/O) ---- */

static Connection *make_conn(void) {
    Connection *conn = calloc(1, sizeof(Connection));
    conn->keep_alive = 1;
    conn->file_fd = -1;
    return conn;
}

static void free_conn(Connection *conn) {
    free(conn->out_buf);
    free(conn);
}

typedef struct {
    char root[PATH_MAX];
    char outside_dir[PATH_MAX];
} StaticFixture;

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    assert(fwrite(content, 1, strlen(content), f) == strlen(content));
    fclose(f);
}

/* Builds a temp directory tree:
 *   <root>/file.txt        - a plain file
 *   <root>/sub/index.html  - directory-index fallback target
 *   <root>/escape          - a symlink pointing outside <root>
 * and canonicalizes <root> via realpath, so tests can build a Route whose
 * static_root matches exactly what app_serve_static would have stored. */
static void setup_fixture(StaticFixture *fx) {
    char root_template[] = "/tmp/cexpress_static_test_XXXXXX";
    char *root = mkdtemp(root_template);
    assert(root != NULL);
    assert(realpath(root, fx->root) != NULL);

    char outside_template[] = "/tmp/cexpress_static_outside_XXXXXX";
    char *outside = mkdtemp(outside_template);
    assert(outside != NULL);
    assert(realpath(outside, fx->outside_dir) != NULL);

    char path[PATH_MAX + 32];
    snprintf(path, sizeof(path), "%s/file.txt", fx->root);
    write_file(path, "hello static\n");

    snprintf(path, sizeof(path), "%s/sub", fx->root);
    assert(mkdir(path, 0755) == 0);
    snprintf(path, sizeof(path), "%s/sub/index.html", fx->root);
    write_file(path, "<h1>index</h1>");

    char secret_path[PATH_MAX + 32];
    snprintf(secret_path, sizeof(secret_path), "%s/secret.txt", fx->outside_dir);
    write_file(secret_path, "should never be served\n");

    snprintf(path, sizeof(path), "%s/escape", fx->root);
    assert(symlink(secret_path, path) == 0);
}

static void teardown_fixture(const StaticFixture *fx) {
    char path[PATH_MAX + 32];
    snprintf(path, sizeof(path), "%s/file.txt", fx->root);
    unlink(path);
    snprintf(path, sizeof(path), "%s/sub/index.html", fx->root);
    unlink(path);
    snprintf(path, sizeof(path), "%s/sub", fx->root);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/escape", fx->root);
    unlink(path);
    rmdir(fx->root);

    snprintf(path, sizeof(path), "%s/secret.txt", fx->outside_dir);
    unlink(path);
    rmdir(fx->outside_dir);
}

static Route make_static_route(const StaticFixture *fx) {
    Route route;
    memset(&route, 0, sizeof(route));
    strncpy(route.method, "GET", sizeof(route.method) - 1);
    strncpy(route.path, "/static/*", sizeof(route.path) - 1);
    memcpy(route.static_root, fx->root, strlen(fx->root) + 1);
    return route;
}

static Request make_request(const char *path) {
    Request req;
    memset(&req, 0, sizeof(req));
    strncpy(req.method, "GET", sizeof(req.method) - 1);
    strncpy(req.path, path, sizeof(req.path) - 1);
    return req;
}

static void test_serve_existing_file(void) {
    StaticFixture fx;
    setup_fixture(&fx);
    Route route = make_static_route(&fx);
    Request req = make_request("/static/file.txt");
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    static_serve_file(&route, &req, &res);

    assert(res.status == 200);
    assert(conn->out_buf != NULL);
    assert(strstr(conn->out_buf, "Content-Type: text/plain\r\n") != NULL);
    assert(strstr(conn->out_buf, "hello static\n") != NULL);

    free_conn(conn);
    teardown_fixture(&fx);
}

static void test_serve_missing_file_404(void) {
    StaticFixture fx;
    setup_fixture(&fx);
    Route route = make_static_route(&fx);
    Request req = make_request("/static/does-not-exist.txt");
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    static_serve_file(&route, &req, &res);

    assert(res.status == 404);

    free_conn(conn);
    teardown_fixture(&fx);
}

static void test_serve_traversal_attempt_403(void) {
    StaticFixture fx;
    setup_fixture(&fx);
    Route route = make_static_route(&fx);
    Request req = make_request("/static/../../etc/passwd");
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    static_serve_file(&route, &req, &res);

    assert(res.status == 403);

    free_conn(conn);
    teardown_fixture(&fx);
}

static void test_serve_symlink_escape_403(void) {
    StaticFixture fx;
    setup_fixture(&fx);
    Route route = make_static_route(&fx);
    Request req = make_request("/static/escape");
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    static_serve_file(&route, &req, &res);

    assert(res.status == 403);

    free_conn(conn);
    teardown_fixture(&fx);
}

static void test_serve_directory_falls_back_to_index(void) {
    StaticFixture fx;
    setup_fixture(&fx);
    Route route = make_static_route(&fx);
    Request req = make_request("/static/sub");
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    static_serve_file(&route, &req, &res);

    assert(res.status == 200);
    assert(conn->out_buf != NULL);
    assert(strstr(conn->out_buf, "Content-Type: text/html\r\n") != NULL);
    assert(strstr(conn->out_buf, "<h1>index</h1>") != NULL);

    free_conn(conn);
    teardown_fixture(&fx);
}

int main(void) {
    test_resolve_literal_subpath();
    test_resolve_root_mount();
    test_resolve_rejects_dotdot_at_start();
    test_resolve_rejects_dotdot_in_middle();
    test_resolve_rejects_dotdot_at_end();
    test_resolve_rejects_path_outside_mount();
    test_resolve_rejects_bare_mount_path();
    test_resolve_rejects_buffer_too_small();
    test_mime_type_known_extensions();
    test_mime_type_defaults_to_octet_stream();
    test_serve_existing_file();
    test_serve_missing_file_404();
    test_serve_traversal_attempt_403();
    test_serve_symlink_escape_403();
    test_serve_directory_falls_back_to_index();

    printf("all static tests passed\n");
    return 0;
}
