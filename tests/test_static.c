#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "app_types.h"
#include "static.h"
#include "response.h"

/* ---- static_serve_file's in-memory cache ---- */

static void sleep_past_revalidate_window(void) {
    /* STATIC_CACHE_REVALIDATE_SECONDS is 1 (app_types.h); comfortably clear it. */
    sleep(2);
}

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

static void test_resolve_hides_dot_segments(void) {
    char out[256];
    assert(static_resolve_relative_path("/static/*", "/static/.env", out, sizeof(out)) == -2);
    assert(static_resolve_relative_path("/static/*", "/static/.git/config", out, sizeof(out)) == -2);
    assert(static_resolve_relative_path("/static/*", "/static/a/.htpasswd", out, sizeof(out)) == -2);
    assert(static_resolve_relative_path("/static/*", "/static/a/.b/c.txt", out, sizeof(out)) == -2);
    assert(static_resolve_relative_path("/static/*", "/static/./file.txt", out, sizeof(out)) == -2);
    assert(static_resolve_relative_path("/static/*", "/static/..", out, sizeof(out)) == -1); /* still 403 */
    /* a dot anywhere but a segment's first byte is an ordinary name */
    assert(static_resolve_relative_path("/static/*", "/static/app.min.js", out, sizeof(out)) == 0);
    assert(strcmp(out, "app.min.js") == 0);
    assert(static_resolve_relative_path("/static/*", "/static/a./b.", out, sizeof(out)) == 0);
    assert(strcmp(out, "a./b.") == 0);
    /* a dot-directory above the mount is the root's business, not the request's */
    assert(static_resolve_relative_path("/.well/*", "/.well/x.txt", out, sizeof(out)) == 0);
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

#include "arena.h"

static Connection *make_conn(void) {
    Connection *conn = calloc(1, sizeof(Connection));
    /* Connection.arena is a pointer to a shared per-worker Arena (App.arena in the real engine)
     * now, not one embedded per connection - malloc a standalone Arena for this fixture to point at. */
    Arena *arena = malloc(sizeof(Arena));
    arena_init(arena, malloc(64 * 1024), 64 * 1024);
    conn->arena = arena;
    conn->keep_alive = 1;
    conn->file_fd = -1;
    return conn;
}

/* What flush_connection would put on the wire: out_buf, then the pinned shared_body (res_send_shared)
 * if any, NUL-terminated in a static buffer so tests can strstr it. */
static const char *sent_text(const Connection *conn) {
    static char wire[STATIC_CACHE_MAX_ENTRY_BYTES + 16 * 1024];
    size_t len = 0;
    if (conn->out_buf != NULL) {
        assert(conn->out_len - conn->out_sent < sizeof(wire));
        memcpy(wire, conn->out_buf + conn->out_sent, conn->out_len - conn->out_sent);
        len = conn->out_len - conn->out_sent;
    }
    if (conn->shared_body != NULL) {
        const size_t body_left = conn->shared_body->len - conn->shared_body_sent;
        assert(len + body_left < sizeof(wire));
        memcpy(wire + len, conn->shared_body->data + conn->shared_body_sent, body_left);
        len += body_left;
    }
    wire[len] = '\0';
    return wire;
}

static void free_conn(Connection *conn) {
    shared_body_detach(conn);
    void *buf = conn->arena->buf;
    arena_reset(conn->arena);
    free(buf);
    free(conn->arena);
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

static Route make_static_route(StaticFixture *fx) {
    Route route;
    memset(&route, 0, sizeof(route));
    strncpy(route.method, "GET", sizeof(route.method) - 1);
    strncpy(route.path, "/static/*", sizeof(route.path) - 1);
    route.static_root = fx->root; /* borrowed: this Route is never passed to route_free */
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
    assert(strstr(sent_text(conn), "Content-Type: text/plain\r\n") != NULL);
    assert(strstr(sent_text(conn), "hello static\n") != NULL);

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

/* .env and .git/config exist under the root and are still 404 - the same answer as a missing file, so
 * their existence is not revealed. Every answer, 200 or not, carries nosniff. */
static void test_serve_dotfiles_404(void) {
    StaticFixture fx;
    setup_fixture(&fx);
    char path[PATH_MAX + 32];
    snprintf(path, sizeof(path), "%s/.env", fx.root);
    write_file(path, "SECRET_KEY=hunter2\n");
    snprintf(path, sizeof(path), "%s/.git", fx.root);
    assert(mkdir(path, 0755) == 0);
    snprintf(path, sizeof(path), "%s/.git/config", fx.root);
    write_file(path, "[core]\n");
    Route route = make_static_route(&fx);

    const char *hidden[] = {"/static/.env", "/static/.git/config", "/static/.git"};
    for (size_t i = 0; i < sizeof(hidden) / sizeof(hidden[0]); i++) {
        Request req = make_request(hidden[i]);
        Connection *conn = make_conn();
        Response res = { .conn = conn, .status = 0 };
        static_serve_file(&route, &req, &res);
        assert(res.status == 404);
        assert(strstr(sent_text(conn), "SECRET_KEY") == NULL && strstr(sent_text(conn), "[core]") == NULL);
        assert(strstr(sent_text(conn), "X-Content-Type-Options: nosniff\r\n") != NULL);
        free_conn(conn);
    }

    Request req = make_request("/static/file.txt");
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };
    static_serve_file(&route, &req, &res);
    assert(res.status == 200);
    assert(strstr(sent_text(conn), "X-Content-Type-Options: nosniff\r\n") != NULL);
    free_conn(conn);

    snprintf(path, sizeof(path), "%s/.git/config", fx.root);
    unlink(path);
    snprintf(path, sizeof(path), "%s/.git", fx.root);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/.env", fx.root);
    unlink(path);
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
    assert(strstr(sent_text(conn), "Content-Type: text/html\r\n") != NULL);
    assert(strstr(sent_text(conn), "<h1>index</h1>") != NULL);

    free_conn(conn);
    teardown_fixture(&fx);
}

static void test_cache_serves_stale_content_within_revalidate_window(void) {
    StaticFixture fx;
    setup_fixture(&fx);
    Route route = make_static_route(&fx);
    Request req = make_request("/static/file.txt");
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    static_serve_file(&route, &req, &res); /* populates the cache */
    assert(res.status == 200);
    assert(strstr(sent_text(conn), "hello static\n") != NULL);

    /* Overwrite the file with different content and, separately, prove the cache - not the filesystem -
     * is what answered the second request by removing the file outright: within the revalidation
     * window, static_serve_file must never touch the filesystem again. */
    char path[PATH_MAX + 32];
    snprintf(path, sizeof(path), "%s/file.txt", fx.root);
    unlink(path);

    Response res2 = { .conn = conn, .status = 0 };
    static_serve_file(&route, &req, &res2);
    assert(res2.status == 200);
    assert(strstr(sent_text(conn), "hello static\n") != NULL);

    /* Restore the file so teardown_fixture's own unlink doesn't fail. */
    write_file(path, "hello static\n");

    free_conn(conn);
    teardown_fixture(&fx);
    static_cache_clear();
}

static void test_cache_revalidates_after_window_and_serves_unchanged_content(void) {
    StaticFixture fx;
    setup_fixture(&fx);
    Route route = make_static_route(&fx);
    Request req = make_request("/static/file.txt");
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    static_serve_file(&route, &req, &res);
    assert(res.status == 200);

    sleep_past_revalidate_window();

    /* File untouched: the mtime/size revalidation should find it unchanged and reuse the cached bytes
     * (this exercises the "existing, but stale check" branch, not the fast within-window one above). */
    Response res2 = { .conn = conn, .status = 0 };
    static_serve_file(&route, &req, &res2);
    assert(res2.status == 200);
    assert(strstr(sent_text(conn), "hello static\n") != NULL);

    free_conn(conn);
    teardown_fixture(&fx);
    static_cache_clear();
}

static void test_cache_picks_up_change_after_window_expires(void) {
    StaticFixture fx;
    setup_fixture(&fx);
    Route route = make_static_route(&fx);
    Request req = make_request("/static/file.txt");
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    static_serve_file(&route, &req, &res);
    assert(res.status == 200);
    assert(strstr(sent_text(conn), "hello static\n") != NULL);

    sleep_past_revalidate_window();

    char path[PATH_MAX + 32];
    snprintf(path, sizeof(path), "%s/file.txt", fx.root);
    write_file(path, "updated content, different size\n");

    Response res2 = { .conn = conn, .status = 0 };
    static_serve_file(&route, &req, &res2);
    assert(res2.status == 200);
    assert(strstr(sent_text(conn), "updated content, different size\n") != NULL);
    assert(strstr(sent_text(conn), "hello static\n") == NULL);

    free_conn(conn);
    teardown_fixture(&fx);
    static_cache_clear();
}

/* Writes `size` bytes of a repeating pattern to <root>/<name>. */
static void write_sized_file(const StaticFixture *fx, const char *name, const size_t size) {
    char path[PATH_MAX + 32];
    snprintf(path, sizeof(path), "%s/%s", fx->root, name);
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    for (size_t i = 0; i < size; i++) {
        assert(fputc('a' + (int)(i % 26), f) != EOF);
    }
    fclose(f);
}

static void remove_fixture_file(const StaticFixture *fx, const char *name) {
    char path[PATH_MAX + 32];
    snprintf(path, sizeof(path), "%s/%s", fx->root, name);
    unlink(path);
}

/* A file past STATIC_CACHE_MAX_ENTRY_BYTES is streamed from disk: the response holds only the head,
 * with the file's full Content-Length, and the connection holds an open fd for the whole body - never
 * the file's bytes in memory (it used to be read whole and copied up to three times per request).
 * A file of exactly the cap is still read whole and cached. */
static void test_large_file_is_streamed_not_buffered(void) {
    StaticFixture fx;
    setup_fixture(&fx);
    const size_t big = (size_t)STATIC_CACHE_MAX_ENTRY_BYTES + 1;
    write_sized_file(&fx, "big.bin", big);
    write_sized_file(&fx, "edge.bin", STATIC_CACHE_MAX_ENTRY_BYTES);
    Route route = make_static_route(&fx);

    Request req = make_request("/static/big.bin");
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };
    static_serve_file(&route, &req, &res);
    assert(res.status == 200);
    assert(conn->file_fd >= 0);
    assert(conn->file_remaining == big);
    assert(conn->out_buf != NULL);
    assert(conn->out_len < 512); /* the head only */
    char cl[64];
    snprintf(cl, sizeof(cl), "Content-Length: %zu\r\n", big);
    assert(strstr(sent_text(conn), cl) != NULL);
    assert(strstr(sent_text(conn), "Content-Type: application/octet-stream\r\n") != NULL);
    char first[16];
    assert(read(conn->file_fd, first, sizeof(first)) == (ssize_t)sizeof(first));
    assert(memcmp(first, "abcdefghijklmnop", sizeof(first)) == 0);
    close(conn->file_fd);
    free_conn(conn);

    /* the cap itself: cached in full and sent by reference (head in out_buf, body pinned), no fd */
    req = make_request("/static/edge.bin");
    conn = make_conn();
    Response res2 = { .conn = conn, .status = 0 };
    static_serve_file(&route, &req, &res2);
    assert(res2.status == 200);
    assert(conn->file_fd == -1);
    assert(conn->out_len < 512);
    assert(conn->shared_body != NULL && conn->shared_body->len == (size_t)STATIC_CACHE_MAX_ENTRY_BYTES);
    free_conn(conn);

    /* HEAD on a large file: head only, no fd kept open */
    req = make_request("/static/big.bin");
    strncpy(req.method, "HEAD", sizeof(req.method) - 1);
    conn = make_conn();
    Response res3 = { .conn = conn, .status = 0, .is_head_request = 1 };
    static_serve_file(&route, &req, &res3);
    assert(res3.status == 200);
    assert(conn->file_fd == -1);
    assert(strstr(sent_text(conn), cl) != NULL);
    free_conn(conn);

    remove_fixture_file(&fx, "big.bin");
    remove_fixture_file(&fx, "edge.bin");
    teardown_fixture(&fx);
    static_cache_clear();
}

/* A cached hit is sent by reference: out_buf holds only the head, every response pins the cache's one
 * SharedBody (no per-request copy), static_cache_clear leaves a pinned body alive, and a later res_* or
 * the connection's end drops the pin. HEAD pins nothing. */
static void test_cached_hit_is_sent_by_reference(void) {
    StaticFixture fx;
    setup_fixture(&fx);
    Route route = make_static_route(&fx);
    Request req = make_request("/static/file.txt");

    Connection *a = make_conn();
    Response res_a = { .conn = a, .status = 0 };
    static_serve_file(&route, &req, &res_a); /* miss: read, cached, sent by reference */
    Connection *b = make_conn();
    Response res_b = { .conn = b, .status = 0 };
    static_serve_file(&route, &req, &res_b); /* fresh hit */

    assert(res_a.status == 200 && res_b.status == 200);
    SharedBody *const body = a->shared_body;
    assert(body != NULL && b->shared_body == body);
    assert(body->refs == 3); /* cache + a + b */
    assert(body->len == strlen("hello static\n") && memcmp(body->data, "hello static\n", body->len) == 0);
    assert(strstr(a->out_buf, "Content-Length: 13\r\n") != NULL);
    assert(strstr(a->out_buf, "hello static") == NULL); /* head only: the body was not copied */
    assert(a->shared_body_sent == 0);

    Request head_req = make_request("/static/file.txt");
    strncpy(head_req.method, "HEAD", sizeof(head_req.method) - 1);
    Connection *h = make_conn();
    Response res_h = { .conn = h, .status = 0, .is_head_request = 1 };
    static_serve_file(&route, &head_req, &res_h);
    assert(h->shared_body == NULL && body->refs == 3);
    assert(strstr(h->out_buf, "Content-Length: 13\r\n") != NULL);
    free_conn(h);

    static_cache_clear();
    assert(body->refs == 2);
    assert(memcmp(body->data, "hello static\n", body->len) == 0); /* still valid (ASan: no use-after-free) */

    Response res_a2 = { .conn = a, .status = 0 };
    res_send(&res_a2, "replaced"); /* last wins: drops a's pin */
    assert(a->shared_body == NULL && body->refs == 1);
    free_conn(a);
    free_conn(b); /* last reference: freed here */

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
    test_resolve_hides_dot_segments();
    test_resolve_rejects_buffer_too_small();
    test_mime_type_known_extensions();
    test_mime_type_defaults_to_octet_stream();
    test_serve_existing_file();
    test_serve_missing_file_404();
    test_serve_dotfiles_404();
    test_serve_traversal_attempt_403();
    test_serve_symlink_escape_403();
    test_serve_directory_falls_back_to_index();
    test_cache_serves_stale_content_within_revalidate_window();
    test_cache_revalidates_after_window_and_serves_unchanged_content();
    test_cache_picks_up_change_after_window_expires();
    test_large_file_is_streamed_not_buffered();
    test_cached_hit_is_sent_by_reference();

    printf("all static tests passed\n");
    return 0;
}
