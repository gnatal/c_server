#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/event.h>
#include "app_types.h"
#include "connection.h"
#include "router.h"
#include "response.h"
#include "middleware.h"

static void ping_handler(const Request *req, Response *res) {
    (void)req;
    res_status(res, 200);
    res_send(res, "pong");
}

static void echo_len_handler(const Request *req, Response *res) {
    char body[64];
    snprintf(body, sizeof(body), "received %d bytes", req->content_length);
    res_status(res, 200);
    res_send(res, body);
}

static void setup_test_connection(App *app, int fds[2], Connection **conn) {
    app_init(app);
    app->kq = kqueue();
    assert(app->kq >= 0);

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    assert(set_nonblocking(fds[0]) == 0);
    assert(set_nonblocking(fds[1]) == 0);

    *conn = connection_create(fds[0]);
    assert(*conn != NULL);
    app->connections[fds[0]] = *conn;
}

static void teardown_test_connection(App *app, int fds[2], Connection *conn) {
    /* app_destroy (connection.h) closes/frees whatever's still tracked in
     * app->connections (including conn, if a test hasn't already closed it
     * itself) plus app->kq - conn is unused directly here now, kept as a
     * parameter so every call site below doesn't need to change. */
    (void)conn;
    close(fds[1]);
    app_destroy(app);
}

static void test_set_nonblocking_and_create(void) {
    int p[2];
    assert(pipe(p) == 0);

    assert(set_nonblocking(p[0]) == 0);
    int flags = fcntl(p[0], F_GETFL, 0);
    assert(flags & O_NONBLOCK);

    Connection *c = connection_create(p[0]);
    assert(c != NULL);
    assert(c->fd == p[0]);
    assert(c->in_buf != NULL);
    assert(c->in_cap == BUF_SIZE);
    assert(c->out_buf == NULL);
    assert(c->in_len == 0);
    assert(c->out_len == 0);

    free(c->in_buf);
    free(c);
    close(p[0]);
    close(p[1]);
}

static void test_handle_readable_round_trip_success(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    app_get(&app, "/ping", ping_handler);

    const char *raw_req = "GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ssize_t written = write(fds[1], raw_req, strlen(raw_req));
    assert(written == (ssize_t)strlen(raw_req));

    handle_readable(&app, conn);

    char resp[1024];
    memset(resp, 0, sizeof(resp));
    ssize_t n = read(fds[1], resp, sizeof(resp) - 1);
    assert(n > 0);
    assert(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    assert(strstr(resp, "pong") != NULL);

    /* Connection is keep-alive by default on HTTP/1.1 */
    assert(app.connections[fds[0]] == conn);
    assert(conn->keep_alive == 1);
    assert(conn->in_len == 0);

    teardown_test_connection(&app, fds, conn);
}

static void test_handle_readable_keep_alive_multiple_requests(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    app_get(&app, "/ping", ping_handler);

    /* First request */
    const char *req1 = "GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\n";
    assert(write(fds[1], req1, strlen(req1)) == (ssize_t)strlen(req1));
    handle_readable(&app, conn);

    char resp1[1024];
    memset(resp1, 0, sizeof(resp1));
    assert(read(fds[1], resp1, sizeof(resp1) - 1) > 0);
    assert(strstr(resp1, "HTTP/1.1 200 OK") != NULL);

    /* Second request on same connection */
    const char *req2 = "GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\n";
    assert(write(fds[1], req2, strlen(req2)) == (ssize_t)strlen(req2));
    handle_readable(&app, conn);

    char resp2[1024];
    memset(resp2, 0, sizeof(resp2));
    assert(read(fds[1], resp2, sizeof(resp2) - 1) > 0);
    assert(strstr(resp2, "HTTP/1.1 200 OK") != NULL);

    teardown_test_connection(&app, fds, conn);
}

static void test_handle_readable_partial_read(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    app_get(&app, "/ping", ping_handler);

    /* Send first partial fragment */
    const char *part1 = "GET /pi";
    assert(write(fds[1], part1, strlen(part1)) == (ssize_t)strlen(part1));
    handle_readable(&app, conn);

    /* Verify no response yet because request is incomplete */
    char buf[128];
    ssize_t n = read(fds[1], buf, sizeof(buf));
    assert(n < 0); /* Non-blocking read would return EAGAIN / EWOULDBLOCK */

    /* Send remaining fragment */
    const char *part2 = "ng HTTP/1.1\r\nHost: localhost\r\n\r\n";
    assert(write(fds[1], part2, strlen(part2)) == (ssize_t)strlen(part2));
    handle_readable(&app, conn);

    /* Now response is available */
    memset(buf, 0, sizeof(buf));
    n = read(fds[1], buf, sizeof(buf) - 1);
    assert(n > 0);
    assert(strstr(buf, "HTTP/1.1 200 OK") != NULL);

    teardown_test_connection(&app, fds, conn);
}

static void test_handle_readable_malformed_400(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    const char *bad_req = "POST / HTTP/1.1\r\nContent-Length: -5\r\n\r\n";
    assert(write(fds[1], bad_req, strlen(bad_req)) == (ssize_t)strlen(bad_req));

    int client_fd = fds[0];
    handle_readable(&app, conn);

    char resp[512];
    memset(resp, 0, sizeof(resp));
    ssize_t n = read(fds[1], resp, sizeof(resp) - 1);
    assert(n > 0);
    assert(strstr(resp, "HTTP/1.1 400 Bad Request") != NULL);

    /* Connection should have been closed */
    assert(app.connections[client_fd] == NULL);

    teardown_test_connection(&app, fds, conn);
}

static void test_handle_readable_unmatched_route_404(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    const char *unmatched_req = "GET /not-a-route HTTP/1.1\r\nHost: localhost\r\n\r\n";
    assert(write(fds[1], unmatched_req, strlen(unmatched_req)) == (ssize_t)strlen(unmatched_req));

    handle_readable(&app, conn);

    char resp[512];
    memset(resp, 0, sizeof(resp));
    ssize_t n = read(fds[1], resp, sizeof(resp) - 1);
    assert(n > 0);
    assert(strstr(resp, "HTTP/1.1 404 Not Found") != NULL);

    teardown_test_connection(&app, fds, conn);
}

static void test_handle_readable_header_overflow_431(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    int client_fd = fds[0];

    /* Fill buffer up to BUF_SIZE - 1 with data that has no \r\n\r\n */
    char large_chunk[1024];
    memset(large_chunk, 'A', sizeof(large_chunk));

    /* Write in chunks until handle_readable detects buffer full */
    for (int i = 0; i < 8; i++) {
        assert(write(fds[1], large_chunk, sizeof(large_chunk)) == sizeof(large_chunk));
        handle_readable(&app, conn);
    }

    char resp[512];
    memset(resp, 0, sizeof(resp));
    ssize_t n = read(fds[1], resp, sizeof(resp) - 1);
    assert(n > 0);
    assert(strstr(resp, "HTTP/1.1 431 Request Header Fields Too Large") != NULL);

    /* Connection should have been closed */
    assert(app.connections[client_fd] == NULL);

    teardown_test_connection(&app, fds, conn);
}

static void test_handle_readable_large_body_grows_buffer(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    app_post(&app, "/upload", echo_len_handler);

    /* Well beyond the original BUF_SIZE (8192), comfortably under
     * MAX_BODY_SIZE - this body cannot possibly fit in in_buf's starting
     * capacity, so completing this request requires handle_readable to grow
     * conn->in_buf partway through (see lib/CLAUDE.md, "Body buffering"). */
    const size_t body_len = 20000;
    char *body = malloc(body_len);
    assert(body != NULL);
    memset(body, 'x', body_len);

    char head[128];
    snprintf(head, sizeof(head),
             "POST /upload HTTP/1.1\r\nHost: localhost\r\nContent-Length: %zu\r\n\r\n", body_len);
    assert(write(fds[1], head, strlen(head)) == (ssize_t)strlen(head));
    handle_readable(&app, conn);
    /* Headers alone fit easily in the starting BUF_SIZE capacity, so no
     * growth should have happened yet - still waiting on the body. */
    assert(conn->in_cap == BUF_SIZE);
    assert(app.connections[fds[0]] == conn);

    /* Stream the body in chunks, like a real socket would deliver it, until
     * the whole thing has been written and handled. */
    int saw_growth = 0;
    size_t sent = 0;
    while (sent < body_len) {
        size_t remaining = body_len - sent;
        size_t chunk = remaining < 4096 ? remaining : 4096;
        ssize_t n = write(fds[1], body + sent, chunk);
        assert(n > 0);
        sent += (size_t)n;
        handle_readable(&app, conn);
        if (conn->in_cap > BUF_SIZE) {
            saw_growth = 1;
        }
    }
    assert(saw_growth);

    char resp[256];
    memset(resp, 0, sizeof(resp));
    ssize_t n = read(fds[1], resp, sizeof(resp) - 1);
    assert(n > 0);
    assert(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    assert(strstr(resp, "received 20000 bytes") != NULL);

    /* Keep-alive connection stays open, and in_buf was shrunk back down to
     * BUF_SIZE now that it's idle again (flush_connection). */
    assert(app.connections[fds[0]] == conn);
    assert(conn->in_cap == BUF_SIZE);

    free(body);
    teardown_test_connection(&app, fds, conn);
}

static void test_handle_readable_body_too_large_413(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    int client_fd = fds[0];

    /* A well-formed but too-large Content-Length is rejected the moment
     * headers complete - no need to actually send MAX_BODY_SIZE+ bytes of
     * body to trigger this (see lib/CLAUDE.md, "Body buffering"). */
    char req_line[128];
    snprintf(req_line, sizeof(req_line),
             "POST /upload HTTP/1.1\r\nContent-Length: %d\r\n\r\n", MAX_BODY_SIZE + 1);
    assert(write(fds[1], req_line, strlen(req_line)) == (ssize_t)strlen(req_line));
    handle_readable(&app, conn);

    char resp[256];
    memset(resp, 0, sizeof(resp));
    ssize_t n = read(fds[1], resp, sizeof(resp) - 1);
    assert(n > 0);
    assert(strstr(resp, "HTTP/1.1 413 Payload Too Large") != NULL);

    /* Connection should have been closed */
    assert(app.connections[client_fd] == NULL);

    teardown_test_connection(&app, fds, conn);
}

static void test_handle_readable_chunked_round_trip(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    app_post(&app, "/upload", echo_len_handler);

    const char *head = "POST /upload HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n";
    assert(write(fds[1], head, strlen(head)) == (ssize_t)strlen(head));
    handle_readable(&app, conn);
    assert(app.connections[fds[0]] == conn);

    /* Two chunks streamed in separately, like a real socket would deliver
     * them, decoding to "Hello World" (11 bytes). */
    const char *chunk1 = "5\r\nHello\r\n";
    const char *chunk2 = "6\r\n World\r\n";
    const char *last_chunk = "0\r\n\r\n";
    assert(write(fds[1], chunk1, strlen(chunk1)) == (ssize_t)strlen(chunk1));
    handle_readable(&app, conn);
    assert(write(fds[1], chunk2, strlen(chunk2)) == (ssize_t)strlen(chunk2));
    handle_readable(&app, conn);
    assert(write(fds[1], last_chunk, strlen(last_chunk)) == (ssize_t)strlen(last_chunk));
    handle_readable(&app, conn);

    char resp[256];
    memset(resp, 0, sizeof(resp));
    ssize_t n = read(fds[1], resp, sizeof(resp) - 1);
    assert(n > 0);
    assert(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    assert(strstr(resp, "received 11 bytes") != NULL);

    /* Keep-alive connection stays open, same as any other HTTP/1.1 request. */
    assert(app.connections[fds[0]] == conn);
    assert(conn->keep_alive == 1);

    teardown_test_connection(&app, fds, conn);
}

static void test_handle_readable_chunked_grows_buffer(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    app_post(&app, "/upload", echo_len_handler);

    const char *head = "POST /upload HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n";
    assert(write(fds[1], head, strlen(head)) == (ssize_t)strlen(head));
    handle_readable(&app, conn);
    assert(conn->in_cap == BUF_SIZE);

    /* Total decoded size well beyond BUF_SIZE (8192) with no Content-Length
     * to jump straight to an exact target - forces handle_readable to grow
     * conn->in_buf geometrically instead (see lib/CLAUDE.md, "Chunked
     * Transfer-Encoding"), unlike the Content-Length path exercised by
     * test_handle_readable_large_body_grows_buffer above. */
    const size_t chunk_payload_len = 4000;
    const int num_chunks = 5;
    char *payload = malloc(chunk_payload_len);
    assert(payload != NULL);
    memset(payload, 'x', chunk_payload_len);

    int saw_growth = 0;
    for (int i = 0; i < num_chunks; i++) {
        char chunk_head[32];
        snprintf(chunk_head, sizeof(chunk_head), "%zx\r\n", chunk_payload_len);
        assert(write(fds[1], chunk_head, strlen(chunk_head)) == (ssize_t)strlen(chunk_head));
        handle_readable(&app, conn);
        assert(write(fds[1], payload, chunk_payload_len) == (ssize_t)chunk_payload_len);
        handle_readable(&app, conn);
        assert(write(fds[1], "\r\n", 2) == 2);
        handle_readable(&app, conn);
        if (conn->in_cap > BUF_SIZE) {
            saw_growth = 1;
        }
    }
    assert(write(fds[1], "0\r\n\r\n", 5) == 5);
    handle_readable(&app, conn);
    assert(saw_growth);

    char resp[256];
    memset(resp, 0, sizeof(resp));
    ssize_t n = read(fds[1], resp, sizeof(resp) - 1);
    assert(n > 0);
    assert(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    char expected[64];
    snprintf(expected, sizeof(expected), "received %d bytes", (int)(chunk_payload_len * (size_t)num_chunks));
    assert(strstr(resp, expected) != NULL);

    /* Keep-alive connection stays open, and in_buf was shrunk back down to
     * BUF_SIZE now that it's idle again (flush_connection). */
    assert(app.connections[fds[0]] == conn);
    assert(conn->in_cap == BUF_SIZE);

    free(payload);
    teardown_test_connection(&app, fds, conn);
}

static void test_handle_readable_chunked_too_large_413(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    const char *head = "POST /upload HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";
    assert(write(fds[1], head, strlen(head)) == (ssize_t)strlen(head));
    handle_readable(&app, conn);
    assert(app.connections[fds[0]] == conn);

    /* The declared chunk size alone already exceeds MAX_BODY_SIZE - rejected
     * immediately, without needing to actually send that much chunk data
     * (see lib/CLAUDE.md, "Chunked Transfer-Encoding", and the analogous
     * Content-Length case in test_handle_readable_body_too_large_413 above). */
    char chunk_head[32];
    snprintf(chunk_head, sizeof(chunk_head), "%x\r\n", MAX_BODY_SIZE + 1);
    assert(write(fds[1], chunk_head, strlen(chunk_head)) == (ssize_t)strlen(chunk_head));
    handle_readable(&app, conn);

    char resp[256];
    memset(resp, 0, sizeof(resp));
    ssize_t n = read(fds[1], resp, sizeof(resp) - 1);
    assert(n > 0);
    assert(strstr(resp, "HTTP/1.1 413 Payload Too Large") != NULL);
    assert(app.connections[fds[0]] == NULL);

    teardown_test_connection(&app, fds, conn);
}

static void test_handle_readable_chunked_malformed_400(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    const char *raw = "POST /upload HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\nZZ\r\nWiki\r\n0\r\n\r\n";
    assert(write(fds[1], raw, strlen(raw)) == (ssize_t)strlen(raw));
    handle_readable(&app, conn);

    char resp[256];
    memset(resp, 0, sizeof(resp));
    ssize_t n = read(fds[1], resp, sizeof(resp) - 1);
    assert(n > 0);
    assert(strstr(resp, "HTTP/1.1 400 Bad Request") != NULL);
    assert(app.connections[fds[0]] == NULL);

    teardown_test_connection(&app, fds, conn);
}

static void test_handle_readable_chunked_and_content_length_400(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    /* RFC 7230 3.3.3: Transfer-Encoding + Content-Length together is
     * ambiguous/smuggling-shaped and rejected outright - see lib/CLAUDE.md,
     * "Chunked Transfer-Encoding". */
    const char *raw = "POST /upload HTTP/1.1\r\nTransfer-Encoding: chunked\r\n"
                       "Content-Length: 4\r\n\r\n4\r\nWiki\r\n0\r\n\r\n";
    assert(write(fds[1], raw, strlen(raw)) == (ssize_t)strlen(raw));
    handle_readable(&app, conn);

    char resp[256];
    memset(resp, 0, sizeof(resp));
    ssize_t n = read(fds[1], resp, sizeof(resp) - 1);
    assert(n > 0);
    assert(strstr(resp, "HTTP/1.1 400 Bad Request") != NULL);
    assert(app.connections[fds[0]] == NULL);

    teardown_test_connection(&app, fds, conn);
}

static void test_handle_readable_path_too_long_414(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    int client_fd = fds[0];

    /* A request-line path longer than req->path (256 bytes) can hold is
     * rejected with 414 rather than silently truncated (see
     * lib/CLAUDE.md, "Request size limits"). 300 chars is comfortably under
     * BUF_SIZE (8192), so this isn't the 431 header-overflow path. */
    const size_t path_len = 300;
    char *path = malloc(path_len + 1);
    path[0] = '/';
    memset(path + 1, 'a', path_len - 1);
    path[path_len] = '\0';

    char *req_line = malloc(4 + path_len + 13 + 1);
    snprintf(req_line, 4 + path_len + 13 + 1, "GET %s HTTP/1.1\r\n\r\n", path);
    assert(write(fds[1], req_line, strlen(req_line)) == (ssize_t)strlen(req_line));
    free(path);
    free(req_line);

    handle_readable(&app, conn);

    char resp[256];
    memset(resp, 0, sizeof(resp));
    ssize_t n = read(fds[1], resp, sizeof(resp) - 1);
    assert(n > 0);
    assert(strstr(resp, "HTTP/1.1 414 URI Too Long") != NULL);

    /* Connection should have been closed */
    assert(app.connections[client_fd] == NULL);

    teardown_test_connection(&app, fds, conn);
}

static void test_handle_readable_connection_close(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    app_get(&app, "/ping", ping_handler);

    const char *close_req = "GET /ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    assert(write(fds[1], close_req, strlen(close_req)) == (ssize_t)strlen(close_req));

    int client_fd = fds[0];
    handle_readable(&app, conn);

    char resp[512];
    memset(resp, 0, sizeof(resp));
    ssize_t n = read(fds[1], resp, sizeof(resp) - 1);
    assert(n > 0);
    assert(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    assert(strstr(resp, "Connection: close") != NULL);

    /* Since Connection: close was requested, connection should be torn down */
    assert(app.connections[client_fd] == NULL);

    teardown_test_connection(&app, fds, conn);
}

static void test_close_idle_connections_closes_stale_keep_alive(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    int client_fd = fds[0];
    /* Idle keep-alive connection (in_len == 0) that's been quiet well past
     * IDLE_TIMEOUT_SECONDS. */
    conn->last_activity = time(NULL) - IDLE_TIMEOUT_SECONDS - 1;

    close_idle_connections(&app);

    /* Nothing to respond to - just reclaimed. */
    assert(app.connections[client_fd] == NULL);
    char buf[16];
    ssize_t n = read(fds[1], buf, sizeof(buf));
    assert(n == 0); /* EOF - fds[0] was closed with nothing written back */

    close(fds[1]);
    close(app.kq);
}

static void test_close_idle_connections_408s_stalled_partial_request(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    int client_fd = fds[0];

    /* A request that started but stalled mid-header. */
    const char *partial = "GET /pi";
    assert(write(fds[1], partial, strlen(partial)) == (ssize_t)strlen(partial));
    handle_readable(&app, conn);
    assert(conn->in_len > 0);

    conn->last_activity = time(NULL) - IDLE_TIMEOUT_SECONDS - 1;
    close_idle_connections(&app);

    char resp[256];
    memset(resp, 0, sizeof(resp));
    ssize_t n = read(fds[1], resp, sizeof(resp) - 1);
    assert(n > 0);
    assert(strstr(resp, "HTTP/1.1 408 Request Timeout") != NULL);
    assert(app.connections[client_fd] == NULL);

    close(fds[1]);
    close(app.kq);
}

static void test_close_idle_connections_leaves_recent_activity_alone(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    conn->last_activity = time(NULL);

    close_idle_connections(&app);

    assert(app.connections[fds[0]] == conn);

    teardown_test_connection(&app, fds, conn);
}

static void test_close_idle_connections_skips_pending_write(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    /* Simulate a write still in flight (e.g. a slow reader on the response
     * side) - this is a different axis than the read-side idle timeout and
     * must not be torn down by close_idle_connections. */
    conn->out_buf = malloc(4);
    assert(conn->out_buf != NULL);
    memcpy(conn->out_buf, "ping", 4);
    conn->out_len = 4;
    conn->out_sent = 0;
    conn->last_activity = time(NULL) - IDLE_TIMEOUT_SECONDS - 1;

    close_idle_connections(&app);

    assert(app.connections[fds[0]] == conn);

    teardown_test_connection(&app, fds, conn);
}

static void test_handle_readable_head_request_omits_body(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    /* No app_head() route registered - this exercises match_route's
     * auto-HEAD-from-GET fallback end to end, not just an explicitly
     * registered HEAD handler. */
    app_get(&app, "/ping", ping_handler);

    const char *raw_req = "HEAD /ping HTTP/1.1\r\nHost: localhost\r\n\r\n";
    assert(write(fds[1], raw_req, strlen(raw_req)) == (ssize_t)strlen(raw_req));

    handle_readable(&app, conn);

    char resp[1024];
    memset(resp, 0, sizeof(resp));
    ssize_t n = read(fds[1], resp, sizeof(resp) - 1);
    assert(n > 0);
    assert(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    /* Content-Length reflects "pong" (4 bytes) - what the equivalent GET
     * would have sent - even though the body itself never went out. */
    assert(strstr(resp, "Content-Length: 4\r\n") != NULL);
    const char *header_end = strstr(resp, "\r\n\r\n");
    assert(header_end != NULL);
    /* Nothing was written to the wire past the header block. */
    assert(header_end + 4 == resp + n);

    teardown_test_connection(&app, fds, conn);
}

static void test_handle_readable_auto_options_response(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    app_get(&app, "/ping", ping_handler);
    app_post(&app, "/ping", ping_handler);

    const char *raw_req = "OPTIONS /ping HTTP/1.1\r\nHost: localhost\r\n\r\n";
    assert(write(fds[1], raw_req, strlen(raw_req)) == (ssize_t)strlen(raw_req));

    handle_readable(&app, conn);

    char resp[1024];
    memset(resp, 0, sizeof(resp));
    ssize_t n = read(fds[1], resp, sizeof(resp) - 1);
    assert(n > 0);
    assert(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    assert(strstr(resp, "Allow: GET, POST, HEAD, OPTIONS\r\n") != NULL);
    assert(strstr(resp, "Content-Length: 0\r\n") != NULL);

    teardown_test_connection(&app, fds, conn);
}

int main(void) {
    test_set_nonblocking_and_create();
    test_handle_readable_round_trip_success();
    test_handle_readable_keep_alive_multiple_requests();
    test_handle_readable_partial_read();
    test_handle_readable_malformed_400();
    test_handle_readable_unmatched_route_404();
    test_handle_readable_header_overflow_431();
    test_handle_readable_large_body_grows_buffer();
    test_handle_readable_body_too_large_413();
    test_handle_readable_chunked_round_trip();
    test_handle_readable_chunked_grows_buffer();
    test_handle_readable_chunked_too_large_413();
    test_handle_readable_chunked_malformed_400();
    test_handle_readable_chunked_and_content_length_400();
    test_handle_readable_path_too_long_414();
    test_handle_readable_connection_close();
    test_close_idle_connections_closes_stale_keep_alive();
    test_close_idle_connections_408s_stalled_partial_request();
    test_close_idle_connections_leaves_recent_activity_alone();
    test_close_idle_connections_skips_pending_write();
    test_handle_readable_head_request_omits_body();
    test_handle_readable_auto_options_response();

    printf("all connection tests passed\n");
    return 0;
}
