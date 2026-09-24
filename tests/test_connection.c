#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "app_types.h"
#include "event_loop.h"
#include "connection.h"
#include "router.h"
#include "response.h"
#include "middleware.h"
#include "http_parser.h"

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

static void auth_header_len_handler(const Request *req, Response *res) {
    const char *auth = req_get_header(req, "Authorization");
    char body[64];
    snprintf(body, sizeof(body), "auth len=%d", auth ? (int)strlen(auth) : -1);
    res_status(res, 200);
    res_send(res, body);
}

static void setup_test_connection(App *app, int fds[2], Connection **conn) {
    app_init(app);
    assert(event_loop_init(app) == 0);

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    assert(set_nonblocking(fds[0]) == 0);
    assert(set_nonblocking(fds[1]) == 0);

    *conn = connection_create(app, fds[0]);
    assert(*conn != NULL);
    app->connections[fds[0]] = *conn;
    assert(event_loop_watch_read(app, fds[0], *conn) == 0);
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

/* Overload tests need real accept_connections() behavior (accept() itself, EMFILE, the listen backlog),
 * which socketpair(2) can't exercise - a real loopback TCP listener is required instead. Binds an
 * ephemeral port (port 0) and initializes the event loop, since accept_connections calls
 * event_loop_watch_read for any connection it doesn't reject. */
static void setup_test_server(App *app) {
    app_init(app);
    assert(event_loop_init(app) == 0);
    app->server_fd = create_server_socket(NULL, 0);
    assert(app->server_fd >= 0);
}

/* A blocking client connect to setup_test_server's app - blocking so the call doesn't return until
 * the client's side of the loopback three-way handshake is done. That does not, it turns out,
 * guarantee the connection is already sitting in the listen backlog: under load (observed
 * occasionally with ASan) accept() can still see nothing for a few milliseconds after connect()
 * returns, so callers should drive accept_connections through drain_accept_connections below rather
 * than a single bare call. */
static int connect_loopback_client(const App *app) {
    struct sockaddr_in bound;
    socklen_t bound_len = sizeof(bound);
    assert(getsockname(app->server_fd, (struct sockaddr *)&bound, &bound_len) == 0);

    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(client_fd >= 0);

    struct sockaddr_in target;
    memset(&target, 0, sizeof(target));
    target.sin_family = AF_INET;
    target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    target.sin_port = bound.sin_port;
    assert(connect(client_fd, (struct sockaddr *)&target, sizeof(target)) == 0);
    return client_fd;
}

/* Calls accept_connections repeatedly with short pauses instead of once, to absorb the rare
 * connect()-returned-but-not-yet-in-the-backlog race described above. Each retry beyond the first is
 * normally a no-op (accept() immediately returns EAGAIN, so accept_connections returns at once) - the
 * pauses only add real time on the rare run that needed them. */
static void drain_accept_connections(App *app) {
    for (int attempt = 0; attempt < 10; attempt++) {
        accept_connections(app);
        struct timespec pause = {0, 5 * 1000 * 1000}; /* 5ms */
        nanosleep(&pause, NULL);
    }
}

static void test_set_nonblocking_and_create(void) {
    int p[2];
    assert(pipe(p) == 0);

    assert(set_nonblocking(p[0]) == 0);
    int flags = fcntl(p[0], F_GETFL, 0);
    assert(flags & O_NONBLOCK);

    /* connection_create points its arena at App.arena now, so it needs one in scope even for a
     * connection this test never registers into app.connections (app_destroy below won't touch it). */
    App app;
    app_init(&app);

    Connection *c = connection_create(&app, p[0]);
    assert(c != NULL);
    assert(c->fd == p[0]);
    assert(c->in_buf == NULL); /* no input memory until a request is partially received */
    assert(c->in_cap == 0);
    assert(c->out_buf == NULL);
    assert(c->in_len == 0);
    assert(c->out_len == 0);

    free(c);
    close(p[0]);
    close(p[1]);
    app_destroy(&app);
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

    const char *bad_req = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: -5\r\n\r\n";
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

static void test_handle_readable_malformed_request_line_400(void) {
    /* a request line picohttpparser rejects outright (here, no HTTP version) used to leave
     * header_len at 0, indistinguishable from "need more bytes" - request_is_complete reported
     * incomplete forever, so the connection just sat there instead of getting an immediate 400
     * (improvements.md's MEASURED reproduction: no reply, socket stays open). */
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    const char *bad_req = "GET /\r\n\r\n";
    assert(write(fds[1], bad_req, strlen(bad_req)) == (ssize_t)strlen(bad_req));

    int client_fd = fds[0];
    handle_readable(&app, conn);

    char resp[512];
    memset(resp, 0, sizeof(resp));
    ssize_t n = read(fds[1], resp, sizeof(resp) - 1);
    assert(n > 0);
    assert(strstr(resp, "HTTP/1.1 400 Bad Request") != NULL);

    /* Connection closed immediately, not left open waiting for headers that will never complete. */
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

/* unlike the whole-header-block-over-BUF_SIZE case above, this is a single header value that used
 * to be too long for its fixed-size MAX_HEADER_VALUE_LEN slot (the old -3 -> 431) even though the request
 * comfortably fits under BUF_SIZE. Now that req->headers holds views into conn->in_buf instead of
 * fixed-size copies, there is no per-header cap left to trip: this exact shape parses successfully and
 * the value round-trips through req_get_header exactly, uncut - the "proper fix" improvements.md said header views
 * would be. */
static void test_handle_readable_long_header_value_is_not_capped(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);
    app_get(&app, "/", auth_header_len_handler);

    int client_fd = fds[0];

    char req_line[1400];
    int n = snprintf(req_line, sizeof(req_line), "GET / HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer ");
    const int token_len = 1100; /* past the old MAX_HEADER_VALUE_LEN (1024) cap */
    for (int i = 0; i < token_len; i++) req_line[n++] = 'x';
    n += snprintf(req_line + n, sizeof(req_line) - (size_t)n, "\r\n\r\n");
    assert(write(fds[1], req_line, (size_t)n) == n);
    handle_readable(&app, conn);

    char resp[256];
    memset(resp, 0, sizeof(resp));
    ssize_t r = read(fds[1], resp, sizeof(resp) - 1);
    assert(r > 0);
    assert(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    char expected[64];
    snprintf(expected, sizeof(expected), "auth len=%d", (int)strlen("Bearer ") + token_len);
    assert(strstr(resp, expected) != NULL);
    assert(app.connections[client_fd] != NULL); /* keep-alive: not rejected, not closed */

    teardown_test_connection(&app, fds, conn);
}

/* "%00" in the request-target used to decode into a real NUL, silently truncating req->path for
 * every C-string function reading it afterward - improvements.md measured "GET /static/style.css%00.png"
 * being routed (and served) as "/static/style.css". parse_http_request now rejects this outright (-4)
 * before routing ever runs, so the connection gets an explicit 400 instead of a 200 for the truncated
 * path. */
static void test_handle_readable_embedded_nul_in_path_400(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    int client_fd = fds[0];

    const char *req_line = "GET /static/style.css%00.png HTTP/1.1\r\nHost: x\r\n\r\n";
    assert(write(fds[1], req_line, strlen(req_line)) == (ssize_t)strlen(req_line));
    handle_readable(&app, conn);

    char resp[256];
    memset(resp, 0, sizeof(resp));
    ssize_t r = read(fds[1], resp, sizeof(resp) - 1);
    assert(r > 0);
    assert(strstr(resp, "HTTP/1.1 400") != NULL);
    assert(app.connections[client_fd] == NULL);

    teardown_test_connection(&app, fds, conn);
}

/* RFC 9112 §3.2: an HTTP/1.1 request with no Host, or with two, is answered 400 and closed on the
 * real in-place parse path; HTTP/1.0 without Host is still served. */
static void test_handle_readable_host_header_count_400(void) {
    const char *rejected[] = {
        "GET /ping HTTP/1.1\r\n\r\n",
        "GET /ping HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n",
    };
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        App app;
        int fds[2];
        Connection *conn;
        setup_test_connection(&app, fds, &conn);
        const int client_fd = fds[0];

        assert(write(fds[1], rejected[i], strlen(rejected[i])) == (ssize_t)strlen(rejected[i]));
        handle_readable(&app, conn);

        char resp[256];
        memset(resp, 0, sizeof(resp));
        const ssize_t r = read(fds[1], resp, sizeof(resp) - 1);
        assert(r > 0);
        assert(strstr(resp, "HTTP/1.1 400") != NULL);
        assert(app.connections[client_fd] == NULL);

        teardown_test_connection(&app, fds, conn);
    }

    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);
    app_get(&app, "/ping", ping_handler);
    const char *http10 = "GET /ping HTTP/1.0\r\n\r\n";
    assert(write(fds[1], http10, strlen(http10)) == (ssize_t)strlen(http10));
    handle_readable(&app, conn);
    char resp[256];
    memset(resp, 0, sizeof(resp));
    assert(read(fds[1], resp, sizeof(resp) - 1) > 0);
    assert(strstr(resp, "HTTP/1.1 200") != NULL && strstr(resp, "pong") != NULL);
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
     * conn->in_buf partway through (see lib/CLAUDE.md, "Behavior reference, Buffers"). */
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

    /* Keep-alive connection stays open, and the grown in_buf was freed now that nothing is
     * buffered (flush_connection, an idle connection owns no input memory). */
    assert(app.connections[fds[0]] == conn);
    assert(conn->in_buf == NULL);
    assert(conn->in_cap == 0);

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
     * body to trigger this (see lib/CLAUDE.md, "Behavior reference, Buffers"). */
    char req_line[128];
    snprintf(req_line, sizeof(req_line),
             "POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n", MAX_BODY_SIZE + 1);
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

/* a declared Content-Length is no longer reserved in one shot (in_cap jumping straight to
 * header_len + content_length + 1) - in_buf grows by doubling as bytes actually arrive, the same
 * strategy the chunked path already used. Regression for the MEASURED problem in improvements.md
 * (300 connections each declaring a 10 MiB body and sending 9 KB cost +3,000 MB of virtual memory). */
static void test_handle_readable_content_length_grows_geometrically(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    app_post(&app, "/upload", echo_len_handler);

    const int declared_len = 5 * 1024 * 1024; /* 5 MiB declared, well under MAX_BODY_SIZE */
    char head[128];
    snprintf(head, sizeof(head), "POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n", declared_len);
    assert(write(fds[1], head, strlen(head)) == (ssize_t)strlen(head));
    handle_readable(&app, conn);
    assert(conn->in_cap == BUF_SIZE); /* headers alone: no growth yet */

    char *body = malloc((size_t)declared_len);
    assert(body != NULL);
    memset(body, 'x', (size_t)declared_len);

    /* Small writes with a drain (handle_readable) after each, like
     * test_handle_readable_large_body_grows_buffer: a nonblocking socketpair fd can't be counted on
     * to accept a large write in one go without the reader draining in between. */
    size_t sent = 0;
    int checked_growth_stays_small = 0;
    while (sent < (size_t)declared_len) {
        size_t left = (size_t)declared_len - sent;
        size_t piece = left < 4096 ? left : 4096;
        ssize_t n = write(fds[1], body + sent, piece);
        assert(n > 0);
        sent += (size_t)n;
        handle_readable(&app, conn);

        /* The first time in_buf grows past its starting capacity, it must not have jumped anywhere
         * near the full declared size - the old behavior reallocated straight to
         * header_len + declared_len + 1 (>5 MiB) on this very first growth; the fix grows by
         * doubling, so at this point (only a few KB actually received) in_cap should still be a
         * small multiple of BUF_SIZE, nowhere close to what the client merely *claimed* it would
         * send. Checked once, right after the first growth, then left alone as further real growth
         * toward the (genuinely arriving) body is expected and fine. */
        if (!checked_growth_stays_small && conn->in_cap > BUF_SIZE) {
            assert(conn->in_cap < (size_t)declared_len / 4);
            checked_growth_stays_small = 1;
        }
    }
    assert(checked_growth_stays_small); /* the body is large enough that growth must have happened */
    free(body);

    char resp[256];
    memset(resp, 0, sizeof(resp));
    ssize_t n = read(fds[1], resp, sizeof(resp) - 1);
    assert(n > 0);
    assert(strstr(resp, "HTTP/1.1 200 OK") != NULL);

    teardown_test_connection(&app, fds, conn);
}

/* app_use_body_limit rejects a declared Content-Length over a route's configured limit with 413
 * as soon as headers are complete, before any body byte is buffered - conn->in_cap must never grow
 * past BUF_SIZE for a request rejected this way. */
static void test_handle_readable_route_body_limit_413(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    app_post(&app, "/api/uploads/avatar", echo_len_handler);
    app_use_body_limit(&app, "/api/uploads", 1024);

    int client_fd = fds[0];
    char req_line[160];
    snprintf(req_line, sizeof(req_line),
             "POST /api/uploads/avatar HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n", 2048);
    assert(write(fds[1], req_line, strlen(req_line)) == (ssize_t)strlen(req_line));
    handle_readable(&app, conn);

    char resp[256];
    memset(resp, 0, sizeof(resp));
    ssize_t n = read(fds[1], resp, sizeof(resp) - 1);
    assert(n > 0);
    assert(strstr(resp, "HTTP/1.1 413 Payload Too Large") != NULL);
    assert(app.connections[client_fd] == NULL); /* rejected and closed */

    teardown_test_connection(&app, fds, conn);
}

/* Sibling of the above: a body within the configured route limit is unaffected - same request shape,
 * declared length now under the 1024-byte cap instead of over it. */
static void test_handle_readable_route_body_limit_allows_within_limit(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    app_post(&app, "/api/uploads/avatar", echo_len_handler);
    app_use_body_limit(&app, "/api/uploads", 1024);

    const int body_len = 100;
    char body[100];
    memset(body, 'z', sizeof(body));
    char head[160];
    snprintf(head, sizeof(head), "POST /api/uploads/avatar HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n", body_len);
    assert(write(fds[1], head, strlen(head)) == (ssize_t)strlen(head));
    assert(write(fds[1], body, sizeof(body)) == (ssize_t)sizeof(body));
    handle_readable(&app, conn);

    char resp[256];
    memset(resp, 0, sizeof(resp));
    ssize_t n = read(fds[1], resp, sizeof(resp) - 1);
    assert(n > 0);
    assert(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    assert(strstr(resp, "received 100 bytes") != NULL);

    teardown_test_connection(&app, fds, conn);
}

#define CONTINUE_LINE "HTTP/1.1 100 Continue\r\n\r\n"

/* Reads whatever the server has written so far (fds[1] is non-blocking), NUL-terminated; 0 if nothing. */
static size_t read_pending(const int fd, char *out, const size_t cap) {
    const ssize_t n = read(fd, out, cap - 1);
    const size_t got = n > 0 ? (size_t)n : 0;
    out[got] = '\0';
    return got;
}

/* headers with "Expect: 100-continue" and no body yet get exactly one "100 Continue" before the
 * body arrives; the body then arrives over several reads without a second one, and the final response
 * follows. A second request on the same keep-alive connection gets its own (continue_sent reset). */
static void test_handle_readable_expect_continue_sends_100_once(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);
    app_post(&app, "/upload", echo_len_handler);

    for (int round = 0; round < 2; round++) {
        const char *head = "POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\nExpect: 100-continue\r\n\r\n";
        assert(write(fds[1], head, strlen(head)) == (ssize_t)strlen(head));
        handle_readable(&app, conn);

        char resp[256];
        read_pending(fds[1], resp, sizeof(resp));
        assert(strcmp(resp, CONTINUE_LINE) == 0);
        assert(conn->continue_sent == 1);

        assert(write(fds[1], "hello", 5) == 5);
        handle_readable(&app, conn);
        assert(read_pending(fds[1], resp, sizeof(resp)) == 0); /* no second 100, no response yet */

        assert(write(fds[1], "world", 5) == 5);
        handle_readable(&app, conn);
        read_pending(fds[1], resp, sizeof(resp));
        assert(strncmp(resp, "HTTP/1.1 200 OK", 15) == 0);
        assert(strstr(resp, "received 10 bytes") != NULL);
        assert(strstr(resp, "100 Continue") == NULL);
        assert(app.connections[fds[0]] == conn);
        assert(conn->continue_sent == 0);
    }

    teardown_test_connection(&app, fds, conn);
}

/* a chunked upload with Expect gets its 100 too. */
static void test_handle_readable_expect_continue_chunked(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);
    app_post(&app, "/upload", echo_len_handler);

    const char *head = "POST /upload HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nExpect: 100-continue\r\n\r\n";
    assert(write(fds[1], head, strlen(head)) == (ssize_t)strlen(head));
    handle_readable(&app, conn);
    char resp[256];
    read_pending(fds[1], resp, sizeof(resp));
    assert(strcmp(resp, CONTINUE_LINE) == 0);

    const char *body = "5\r\nHello\r\n0\r\n\r\n";
    assert(write(fds[1], body, strlen(body)) == (ssize_t)strlen(body));
    handle_readable(&app, conn);
    read_pending(fds[1], resp, sizeof(resp));
    assert(strncmp(resp, "HTTP/1.1 200 OK", 15) == 0);
    assert(strstr(resp, "received 5 bytes") != NULL);

    teardown_test_connection(&app, fds, conn);
}

/* no 100 when it is not wanted or not allowed - the body already arrived with the headers, the
 * client is HTTP/1.0, or the declared body is over the route limit (413 instead, body never invited). */
static void test_handle_readable_expect_continue_not_sent(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);
    app_post(&app, "/upload", echo_len_handler);
    app_post(&app, "/api/uploads/avatar", echo_len_handler);
    app_use_body_limit(&app, "/api/uploads", 1024);

    /* Body sent without waiting: the request is complete, answered directly. */
    const char *whole = "POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\nhello";
    assert(write(fds[1], whole, strlen(whole)) == (ssize_t)strlen(whole));
    handle_readable(&app, conn);
    char resp[256];
    read_pending(fds[1], resp, sizeof(resp));
    assert(strncmp(resp, "HTTP/1.1 200 OK", 15) == 0);
    assert(strstr(resp, "100 Continue") == NULL);

    /* HTTP/1.0 (keep-alive so the connection survives for the next case). */
    const char *v10 = "POST /upload HTTP/1.0\r\nConnection: keep-alive\r\nContent-Length: 5\r\n"
                      "Expect: 100-continue\r\n\r\n";
    assert(write(fds[1], v10, strlen(v10)) == (ssize_t)strlen(v10));
    handle_readable(&app, conn);
    assert(read_pending(fds[1], resp, sizeof(resp)) == 0);
    assert(write(fds[1], "hello", 5) == 5);
    handle_readable(&app, conn);
    read_pending(fds[1], resp, sizeof(resp));
    assert(strstr(resp, "200 OK") != NULL && strstr(resp, "100 Continue") == NULL);
    assert(app.connections[fds[0]] == conn);

    /* Over the route limit: 413, and the connection closes without a 100. */
    const char *big = "POST /api/uploads/avatar HTTP/1.1\r\nHost: x\r\nContent-Length: 2048\r\nExpect: 100-continue\r\n\r\n";
    assert(write(fds[1], big, strlen(big)) == (ssize_t)strlen(big));
    handle_readable(&app, conn);
    read_pending(fds[1], resp, sizeof(resp));
    assert(strncmp(resp, "HTTP/1.1 413 Payload Too Large", 30) == 0);
    assert(app.connections[fds[0]] == NULL);

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

/* a head written one byte per read advances Connection.head_scan with the buffer (the blank-line
 * search resumes where it left off instead of re-parsing the head from its first byte every read), the
 * request is answered once its blank line arrives, and the keep-alive reset puts head_scan back to 0 so
 * a second request on the same connection searches its own head from the start. */
static void test_handle_readable_head_scan_resumes_and_resets(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    app_get(&app, "/ping", ping_handler);

    const char *req = "GET /ping HTTP/1.1\r\nHost: localhost\r\nX-Pad: abcdefgh\r\n\r\n";
    const size_t len = strlen(req);
    for (size_t i = 0; i + 1 < len; i++) {
        assert(write(fds[1], req + i, 1) == 1);
        handle_readable(&app, conn);
        assert(app.connections[fds[0]] == conn);
        assert(conn->in_len - conn->in_off == i + 1); /* still buffered, unanswered */
        assert(i < 2 || conn->head_scan == i - 1);   /* 2 bytes behind the buffer's end */
    }
    assert(write(fds[1], req + len - 1, 1) == 1);
    handle_readable(&app, conn);

    char resp[256];
    memset(resp, 0, sizeof(resp));
    assert(read(fds[1], resp, sizeof(resp) - 1) > 0);
    assert(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    assert(app.connections[fds[0]] == conn);
    assert(conn->head_scan == 0);

    /* Second, shorter request: a stale head_scan would start past its blank line and never find it. */
    const char *second = "GET /ping HTTP/1.1\r\nHost: x\r\n\r\n";
    assert(write(fds[1], second, strlen(second)) == (ssize_t)strlen(second));
    handle_readable(&app, conn);
    memset(resp, 0, sizeof(resp));
    assert(read(fds[1], resp, sizeof(resp) - 1) > 0);
    assert(strstr(resp, "HTTP/1.1 200 OK") != NULL);

    teardown_test_connection(&app, fds, conn);
}

/* a chunked body dribbled in over several reads advances Connection.chunk_scan past each
 * completed chunk (so no read rescans the body before it), and a keep-alive response resets it -
 * a second chunked request on the same connection must scan from its own body start, not resume at
 * the first request's offset (which would mis-frame it). */
static void test_handle_readable_chunked_scan_resumes_and_resets(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    app_post(&app, "/upload", echo_len_handler);

    const char *head = "POST /upload HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n";
    const char *pieces[] = {"5\r\nHel", "lo\r\n6\r\n World\r\n", "0\r\n", "\r\n"};
    assert(write(fds[1], head, strlen(head)) == (ssize_t)strlen(head));
    handle_readable(&app, conn);
    assert(conn->chunk_scan.pos == 0);

    assert(write(fds[1], pieces[0], strlen(pieces[0])) == (ssize_t)strlen(pieces[0]));
    handle_readable(&app, conn);
    assert(conn->chunk_scan.pos == 0); /* first chunk's data still incomplete */

    assert(write(fds[1], pieces[1], strlen(pieces[1])) == (ssize_t)strlen(pieces[1]));
    handle_readable(&app, conn);
    assert(conn->chunk_scan.pos == strlen("5\r\nHello\r\n6\r\n World\r\n"));
    assert(conn->chunk_scan.decoded_len == 11);

    assert(write(fds[1], pieces[2], strlen(pieces[2])) == (ssize_t)strlen(pieces[2]));
    handle_readable(&app, conn);
    assert(write(fds[1], pieces[3], strlen(pieces[3])) == (ssize_t)strlen(pieces[3]));
    handle_readable(&app, conn);

    char resp[256];
    memset(resp, 0, sizeof(resp));
    assert(read(fds[1], resp, sizeof(resp) - 1) > 0);
    assert(strstr(resp, "received 11 bytes") != NULL);
    assert(app.connections[fds[0]] == conn);
    assert(conn->chunk_scan.pos == 0);
    assert(conn->chunk_scan.decoded_len == 0);
    assert(conn->chunk_scan.trailer_from == 0);

    /* Second request, shorter body: a stale pos (22) would point past its whole body. */
    const char *second = "POST /upload HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n"
                         "3\r\nabc\r\n0\r\n\r\n";
    assert(write(fds[1], second, strlen(second)) == (ssize_t)strlen(second));
    handle_readable(&app, conn);
    memset(resp, 0, sizeof(resp));
    assert(read(fds[1], resp, sizeof(resp) - 1) > 0);
    assert(strstr(resp, "received 3 bytes") != NULL);

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
     * conn->in_buf geometrically instead (see lib/CLAUDE.md, "Behavior reference, Request parsing"), unlike the Content-Length path exercised by
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

    /* Keep-alive connection stays open, and the grown in_buf was freed now that nothing is
     * buffered (flush_connection, an idle connection owns no input memory). */
    assert(app.connections[fds[0]] == conn);
    assert(conn->in_buf == NULL);
    assert(conn->in_cap == 0);

    free(payload);
    teardown_test_connection(&app, fds, conn);
}

static void test_handle_readable_chunked_too_large_413(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    const char *head = "POST /upload HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n";
    assert(write(fds[1], head, strlen(head)) == (ssize_t)strlen(head));
    handle_readable(&app, conn);
    assert(app.connections[fds[0]] == conn);

    /* The declared chunk size alone already exceeds MAX_BODY_SIZE - rejected
     * immediately, without needing to actually send that much chunk data
     * (see lib/CLAUDE.md, "Behavior reference, Request parsing", and the analogous
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

    const char *raw = "POST /upload HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\nZZ\r\nWiki\r\n0\r\n\r\n";
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
     * "Behavior reference, Request parsing". */
    const char *raw = "POST /upload HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n"
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

static void test_handle_readable_bare_lf_400(void) {
    /* picohttpparser tolerates a bare '\n' as a line terminator, a leniency a strict front proxy
     * would not extend - closing that ambiguity end to end through the real connection path. */
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    const char *raw = "GET / HTTP/1.1\nHost: x\r\n\r\n";
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

static void test_handle_readable_unsupported_transfer_encoding_501(void) {
    /* a Transfer-Encoding this engine can't frame (here, "chunked" isn't the value's sole token) is
     * rejected with 501 rather than silently mistaken for plain chunked framing via a substring match. */
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    const char *raw = "POST /upload HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip, chunked\r\n\r\n4\r\nWiki\r\n0\r\n\r\n";
    assert(write(fds[1], raw, strlen(raw)) == (ssize_t)strlen(raw));
    handle_readable(&app, conn);

    char resp[256];
    memset(resp, 0, sizeof(resp));
    ssize_t n = read(fds[1], resp, sizeof(resp) - 1);
    assert(n > 0);
    assert(strstr(resp, "HTTP/1.1 501 Not Implemented") != NULL);
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
     * lib/CLAUDE.md, "Limits"). 300 chars is comfortably under
     * BUF_SIZE (8192), so this isn't the 431 header-overflow path. */
    const size_t path_len = 300;
    char *path = malloc(path_len + 1);
    path[0] = '/';
    memset(path + 1, 'a', path_len - 1);
    path[path_len] = '\0';

    char *req_line = malloc(4 + path_len + 22 + 1);
    snprintf(req_line, 4 + path_len + 22 + 1, "GET %s HTTP/1.1\r\nHost: x\r\n\r\n", path);
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
    app_destroy(&app);
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
    /* handle_readable already started the request_started clock; back it up past
     * IDLE_TIMEOUT_SECONDS too so this exercises the pre-existing "quiet past IDLE_TIMEOUT_SECONDS"
     * shape (bigger than either request deadline) rather than the new, tighter one. */
    conn->request_started = time(NULL) - IDLE_TIMEOUT_SECONDS - 1;
    close_idle_connections(&app);

    char resp[256];
    memset(resp, 0, sizeof(resp));
    ssize_t n = read(fds[1], resp, sizeof(resp) - 1);
    assert(n > 0);
    assert(strstr(resp, "HTTP/1.1 408 Request Timeout") != NULL);
    assert(app.connections[client_fd] == NULL);

    close(fds[1]);
    app_destroy(&app);
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

    /* Simulate a write still in flight that is actively making progress (e.g. a slow but real
     * reader on the response side) - this is a different axis than the read-side idle timeout, and
     * must not be torn down by close_idle_connections as long as last_write_progress is recent. */
    conn->out_buf = malloc(4);
    assert(conn->out_buf != NULL);
    memcpy(conn->out_buf, "ping", 4);
    conn->out_len = 4;
    conn->out_sent = 0;
    conn->last_activity = time(NULL) - IDLE_TIMEOUT_SECONDS - 1;
    conn->last_write_progress = time(NULL);

    close_idle_connections(&app);

    assert(app.connections[fds[0]] == conn);

    teardown_test_connection(&app, fds, conn);
}

/* a client that requests a response and then stops reading (never a byte accepted onto the
 * socket) used to be exempted from close_idle_connections entirely ("slow readers are not this
 * timeout's job"), pinning the fd, arena and out_buf forever. last_write_progress bounds that. */
static void test_close_idle_connections_write_stall_closes_connection(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    int client_fd = fds[0];

    conn->out_buf = malloc(4);
    assert(conn->out_buf != NULL);
    memcpy(conn->out_buf, "ping", 4);
    conn->out_len = 4;
    conn->out_sent = 0;
    conn->last_write_progress = time(NULL) - WRITE_TIMEOUT_SECONDS - 1;

    close_idle_connections(&app);

    assert(app.connections[client_fd] == NULL); /* reclaimed: nothing more to say to a reader that stopped reading */

    close(fds[1]);
    app_destroy(&app);
}


/* a client that sends one byte every few seconds keeps refreshing last_activity forever, so the
 * old idle-only check (last_activity vs IDLE_TIMEOUT_SECONDS) never fires. request_started does not
 * reset on each byte, so it bounds total time-to-complete-headers regardless. */
static void test_close_idle_connections_header_deadline_closes_slow_drip(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    int client_fd = fds[0];

    const char *partial = "GET /p";
    assert(write(fds[1], partial, strlen(partial)) == (ssize_t)strlen(partial));
    handle_readable(&app, conn); /* starts conn->request_started */
    assert(conn->in_len > 0);

    /* A byte "just" arrived (last_activity is fresh - the slow-drip trick), but the request has
     * actually been dribbling in for longer than the header deadline allows. */
    conn->last_activity = time(NULL);
    conn->request_started = time(NULL) - REQUEST_HEADER_TIMEOUT_SECONDS - 1;

    close_idle_connections(&app);

    char resp[256];
    memset(resp, 0, sizeof(resp));
    ssize_t n = read(fds[1], resp, sizeof(resp) - 1);
    assert(n > 0);
    assert(strstr(resp, "HTTP/1.1 408 Request Timeout") != NULL);
    assert(app.connections[client_fd] == NULL);

    close(fds[1]);
    app_destroy(&app);
}

/* A partial request still within the header deadline must be left alone, even though it would have
 * tripped the old, much longer IDLE_TIMEOUT_SECONDS-only sweep eventually. */
static void test_close_idle_connections_header_deadline_leaves_fresh_partial_request_alone(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    const char *partial = "GET /p";
    assert(write(fds[1], partial, strlen(partial)) == (ssize_t)strlen(partial));
    handle_readable(&app, conn);
    assert(conn->in_len > 0);
    assert(conn->request_started != 0);

    close_idle_connections(&app);

    assert(app.connections[fds[0]] == conn);

    teardown_test_connection(&app, fds, conn);
}

/* Once headers are complete, a still-pending body gets the more generous body deadline instead of the
 * tight header deadline - a slow but legitimate upload should not be cut off at the header threshold. */
static void test_close_idle_connections_body_deadline_allows_slow_body_within_window(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    app_post(&app, "/upload", echo_len_handler);

    const char *head = "POST /upload HTTP/1.1\r\nHost: localhost\r\nContent-Length: 10\r\n\r\n";
    assert(write(fds[1], head, strlen(head)) == (ssize_t)strlen(head));
    handle_readable(&app, conn); /* headers complete, body still pending */
    assert(conn->in_len > 0);

    /* Past the header deadline but still inside the body deadline - must survive. */
    conn->request_started = time(NULL) - REQUEST_HEADER_TIMEOUT_SECONDS - 1;

    close_idle_connections(&app);

    assert(app.connections[fds[0]] == conn);

    teardown_test_connection(&app, fds, conn);
}

/* A body that never finishes arriving still has to give up eventually: past the (longer) body
 * deadline, close_idle_connections must reject it rather than hold it forever. */
static void test_close_idle_connections_body_deadline_closes_stalled_body(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    int client_fd = fds[0];
    app_post(&app, "/upload", echo_len_handler);

    const char *head = "POST /upload HTTP/1.1\r\nHost: localhost\r\nContent-Length: 10\r\n\r\n";
    assert(write(fds[1], head, strlen(head)) == (ssize_t)strlen(head));
    handle_readable(&app, conn);
    assert(conn->in_len > 0);

    conn->request_started = time(NULL) - REQUEST_BODY_TIMEOUT_SECONDS - 1;

    close_idle_connections(&app);

    char resp[256];
    memset(resp, 0, sizeof(resp));
    ssize_t n = read(fds[1], resp, sizeof(resp) - 1);
    assert(n > 0);
    assert(strstr(resp, "HTTP/1.1 408 Request Timeout") != NULL);
    assert(app.connections[client_fd] == NULL);

    close(fds[1]);
    app_destroy(&app);
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

static void test_app_count_connections(void) {
    App app;
    app_init(&app);
    assert(event_loop_init(&app) == 0);
    assert(app_count_connections(&app) == 0);

    int fds1[2], fds2[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds1) == 0);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds2) == 0);

    Connection *c1 = connection_create(&app, fds1[0]);
    Connection *c2 = connection_create(&app, fds2[0]);
    assert(c1 != NULL && c2 != NULL);

    app.connections[fds1[0]] = c1;
    assert(app_count_connections(&app) == 1);

    app.connections[fds2[0]] = c2;
    assert(app_count_connections(&app) == 2);

    connection_close(&app, c1);
    assert(app_count_connections(&app) == 1);
    assert(app.connections[fds1[0]] == NULL);

    close(fds1[1]);
    close(fds2[1]);
    app_destroy(&app);
    assert(app_count_connections(&app) == 0);
}

static void test_app_stop_idempotency_and_closing_idle(void) {
    App app;
    app_init(&app);
    assert(event_loop_init(&app) == 0);

    /* Dummy server socket using socketpair */
    int srv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, srv) == 0);
    app.server_fd = srv[0];
    event_loop_watch_read(&app, app.server_fd, NULL);

    int fds_idle[2], fds_busy[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds_idle) == 0);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds_busy) == 0);

    Connection *c_idle = connection_create(&app, fds_idle[0]);
    Connection *c_busy = connection_create(&app, fds_busy[0]);
    assert(c_idle != NULL && c_busy != NULL);

    app.connections[fds_idle[0]] = c_idle;
    app.connections[fds_busy[0]] = c_busy;

    /* Simulate c_busy having partial request bytes in flight */
    c_busy->in_len = 10;
    c_busy->keep_alive = 1;

    /* Initiate graceful shutdown */
    app_stop(&app);
    assert(app.is_shutting_down == 1);
    assert(app.server_fd == -1);

    /* Idle connection must be closed immediately */
    assert(app.connections[fds_idle[0]] == NULL);

    /* Busy connection must remain open but with keep_alive cleared */
    assert(app.connections[fds_busy[0]] != NULL);
    assert(c_busy->keep_alive == 0);

    /* Second call must be a no-op (idempotent) */
    app_stop(&app);
    assert(app.is_shutting_down == 1);

    close(srv[1]);
    close(fds_idle[1]);
    close(fds_busy[1]);
    app_destroy(&app);
}

static void test_handle_readable_during_shutdown_forces_connection_close(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);
    app_get(&app, "/ping", ping_handler);

    /* Enter graceful shutdown */
    app.is_shutting_down = 1;

    /* Client sends request with explicit Connection: keep-alive */
    const char *req = "GET /ping HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
    assert(write(fds[1], req, strlen(req)) == (ssize_t)strlen(req));

    handle_readable(&app, conn);

    char resp[1024];
    memset(resp, 0, sizeof(resp));
    ssize_t n = read(fds[1], resp, sizeof(resp) - 1);
    assert(n > 0);
    assert(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    /* During shutdown, Connection header MUST be "close", not "keep-alive" */
    assert(strstr(resp, "Connection: close\r\n") != NULL);
    assert(strstr(resp, "Connection: keep-alive\r\n") == NULL);

    /* Connection should be closed after response finishes, not kept open */
    assert(app.connections[fds[0]] == NULL);
    assert(app_count_connections(&app) == 0);

    close(fds[1]);
    app_destroy(&app);
}

/* Write-stall deadline, end to end through the real flush_connection path (not a manually poked field): a response
 * larger than the socketpair's kernel buffers, with nobody ever reading the peer end, forces a real
 * EAGAIN partway through and proves flush_connection itself arms last_write_progress. Backdating it
 * past WRITE_TIMEOUT_SECONDS and re-running the sweep must then reclaim the stuck connection -
 * exactly the "client stopped reading" case improvements.md flagged as untested. */
static void test_flush_connection_write_stall_reclaimed_by_close_idle_connections(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    const size_t body_len = 8 * 1024 * 1024; /* comfortably larger than any default socket buffer */
    char *body = malloc(body_len);
    assert(body != NULL);
    memset(body, 'x', body_len);
    conn->out_buf = body;
    conn->out_len = body_len;
    conn->out_sent = 0;
    conn->keep_alive = 1;

    flush_connection(&app, conn);

    assert(app.connections[fds[0]] == conn);  /* still open: partial write in flight */
    assert(conn->out_sent < conn->out_len);   /* proves a real EAGAIN was hit, not a full drain */
    assert(conn->last_write_progress != 0);

    /* Peer never reads: simulate the stall running past the deadline. */
    conn->last_write_progress = time(NULL) - WRITE_TIMEOUT_SECONDS - 1;
    close_idle_connections(&app);

    assert(app.connections[fds[0]] == NULL);

    close(fds[1]);
    app_destroy(&app);
    free(body);
}

static void test_app_stop_drains_and_flushes_pending_write(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    /* Set up an in-flight response buffered in conn->out_buf */
    const char *data = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\npong";
    size_t len = strlen(data);
    conn->out_buf = malloc(len);
    assert(conn->out_buf != NULL);
    memcpy(conn->out_buf, data, len);
    conn->out_len = len;
    conn->out_sent = 0;
    conn->keep_alive = 1;

    /* Graceful shutdown should leave conn open because out_buf != NULL,
     * but force keep_alive to 0 */
    app_stop(&app);
    assert(app.connections[fds[0]] != NULL);
    assert(conn->keep_alive == 0);

    /* Flush out the write */
    flush_connection(&app, conn);

    char resp[128];
    memset(resp, 0, sizeof(resp));
    ssize_t n = read(fds[1], resp, sizeof(resp) - 1);
    assert(n == (ssize_t)len);
    assert(strcmp(resp, data) == 0);

    /* Once flushed, the connection is closed */
    assert(app.connections[fds[0]] == NULL);
    assert(app_count_connections(&app) == 0);

    close(fds[1]);
    app_destroy(&app);
}

static void test_res_send_file_streams_to_socket(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_test_connection(&app, fds, &conn);

    char tmp_path[] = "/tmp/cexpress_stream_test_XXXXXX";
    int tmp_fd = mkstemp(tmp_path);
    assert(tmp_fd >= 0);
    const char *payload = "hello streamed file content\n";
    assert(write(tmp_fd, payload, strlen(payload)) == (ssize_t)strlen(payload));
    close(tmp_fd);

    Response res;
    memset(&res, 0, sizeof(res));
    res.conn = conn;
    conn->keep_alive = 0;

    int rc = res_send_file(&res, "text/plain", tmp_path);
    assert(rc == 0);
    assert(conn->file_fd >= 0);

    flush_connection(&app, conn);

    char buf[1024];
    memset(buf, 0, sizeof(buf));
    ssize_t n = read(fds[1], buf, sizeof(buf) - 1);
    assert(n > 0);
    assert(strstr(buf, "HTTP/1.1 200 OK\r\n") != NULL);
    assert(strstr(buf, "Content-Type: text/plain\r\n") != NULL);
    assert(strstr(buf, "hello streamed file content\n") != NULL);

    assert(app.connections[fds[0]] == NULL);

    unlink(tmp_path);
    close(fds[1]);
    app_destroy(&app);
}

/* Finds "\r\n\r\n" in a buffer that isn't NUL-terminated (or safe to strstr - the shared-arena test below streams
 * binary content that legitimately contains NUL bytes) and returns a pointer just past it, or NULL. */
static const char *skip_response_head(const char *buf, size_t len) {
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return buf + i + 4;
        }
    }
    return NULL;
}

/* Shared-arena regression: Connection.arena became a pointer to one Arena shared by every connection a worker
 * serves, instead of one embedded per connection - the risk improvements.md called out for this fix
 * is exactly the scenario built here. A file well over STREAM_CHUNK_SIZE is streamed to conn1 over a
 * socketpair (small enough buffers that flush_connection's own 4*STREAM_CHUNK_SIZE fairness-yield
 * forces several separate calls, each returning control to "the event loop" - this test's own driving
 * loop - well before the file finishes). Between every one of those calls, conn2 runs a completely
 * ordinary dispatch-and-flush cycle of its own on the SAME app, which builds its response from and
 * then resets the shared arena, exactly what happens for real between two connections' turns on one
 * worker. If conn1's streamed chunks lived in that arena (the pre-fix design), conn2's activity would
 * corrupt or truncate them; because they live in Connection.file_buf instead - connection-owned,
 * lazily malloc'd, never arena-resident - the full file must still arrive at the client byte for byte. */
static void test_flush_connection_file_stream_survives_another_connections_dispatch(void) {
    App app;
    app_init(&app);
    assert(event_loop_init(&app) == 0);
    app_get(&app, "/ping", ping_handler);

    int fds1[2], fds2[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds1) == 0);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds2) == 0);
    assert(set_nonblocking(fds1[0]) == 0);
    assert(set_nonblocking(fds1[1]) == 0);
    assert(set_nonblocking(fds2[0]) == 0);
    assert(set_nonblocking(fds2[1]) == 0);

    Connection *conn1 = connection_create(&app, fds1[0]);
    Connection *conn2 = connection_create(&app, fds2[0]);
    assert(conn1 != NULL && conn2 != NULL);
    app.connections[fds1[0]] = conn1;
    app.connections[fds2[0]] = conn2;
    conn1->keep_alive = 0; /* closes on its own once fully sent: doubles as this test's exit condition */
    conn2->keep_alive = 1;

    char tmp_path[] = "/tmp/cexpress_m1_stream_test_XXXXXX";
    int tmp_fd = mkstemp(tmp_path);
    assert(tmp_fd >= 0);
    /* Several times STREAM_CHUNK_SIZE and max_flush_bytes (4x that), so the fairness-yield inside
     * flush_connection fires repeatedly - one interleaving opportunity per yield below. A non-repeating
     * byte pattern (not a short cycle aligned with the 16 KiB chunk size) so a duplicated, dropped or
     * corrupted chunk is detectable rather than masked by a value that looks the same everywhere. */
    const size_t file_len = STREAM_CHUNK_SIZE * 30 + 777;
    char *expected = malloc(file_len);
    assert(expected != NULL);
    for (size_t i = 0; i < file_len; i++) {
        expected[i] = (char)((i * 2654435761u) >> 24); /* Knuth multiplicative hash of the index */
    }
    assert(write(tmp_fd, expected, file_len) == (ssize_t)file_len);
    close(tmp_fd);

    Response res1;
    memset(&res1, 0, sizeof(res1));
    res1.conn = conn1;
    assert(res_send_file(&res1, "application/octet-stream", tmp_path) == 0);
    assert(conn1->file_fd >= 0);

    char *received = malloc(file_len + 4096);
    assert(received != NULL);
    size_t received_len = 0;
    int rounds = 0;
    int interleaved = 0;
    while (app.connections[fds1[0]] != NULL) {
        assert(rounds++ < 10000); /* generous cap: fail loudly instead of hanging if something regresses */

        flush_connection(&app, conn1);

        char chunk[4096];
        ssize_t n;
        while ((n = read(fds1[1], chunk, sizeof(chunk))) > 0) {
            assert(received_len + (size_t)n <= file_len + 4096);
            memcpy(received + received_len, chunk, (size_t)n);
            received_len += (size_t)n;
        }
        if (app.connections[fds1[0]] == NULL) {
            break; /* conn1 fully sent and closed itself (keep_alive was 0) */
        }

        /* Interleave conn2's own, unrelated dispatch-and-flush cycle on the same app - the exact
         * "another connection's turn happens between this one's yields" sequence handle_readable
         * produces for real, reusing the identical shared-arena reset it performs after its own
         * flush_connection call. */
        Request req2;
        const char *raw2 = "GET /ping HTTP/1.1\r\nHost: x\r\n\r\n";
        assert(parse_http_request(raw2, strlen(raw2), &req2, &app.arena) == 0);
        Response res2;
        res_init(&res2, conn2);
        dispatch(&app, match_route(&app, &req2), &req2, &res2);
        flush_connection(&app, conn2);
        arena_reset(&app.arena);
        interleaved++;
        char discard[256];
        while (read(fds2[1], discard, sizeof(discard)) > 0) { }
    }
    assert(app.connections[fds1[0]] == NULL);
    /* If this is 0, the file was too small (or max_flush_bytes/STREAM_CHUNK_SIZE changed) for the
     * fairness-yield to ever fire, and the test below would pass without exercising the shared arena at all. */
    assert(interleaved > 0);

    const char *body = skip_response_head(received, received_len);
    assert(body != NULL);
    size_t body_len = received_len - (size_t)(body - received);
    assert(body_len == file_len);
    assert(memcmp(body, expected, file_len) == 0);

    free(expected);
    free(received);
    unlink(tmp_path);
    close(fds1[1]);
    close(fds2[1]);
    app_destroy(&app);
}

/* accept_client is a single accept/accept4 call with no per-connection fcntl/setsockopt; the fd
 * it returns must still be non-blocking with TCP_NODELAY (inherited from create_server_socket's
 * listener on BSD/macOS; accept4 flags + listener inheritance on Linux, where FD_CLOEXEC comes free
 * too). Fails if a platform stops inheriting either, instead of Nagle or blocking I/O coming back
 * silently. Also covers the admitted Connection end to end through accept_connections. */
static void test_accept_client_socket_options(void) {
    App app;
    setup_test_server(&app);

    int nodelay = 0;
    socklen_t optlen = sizeof(nodelay);
    assert(getsockopt(app.server_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, &optlen) == 0);
    assert(nodelay != 0);

    const int client = connect_loopback_client(&app);
    int accepted = -1;
    for (int attempt = 0; attempt < 10 && accepted < 0; attempt++) {
        accepted = accept_client(app.server_fd);
        if (accepted < 0) {
            assert(errno == EAGAIN || errno == EWOULDBLOCK);
            struct timespec pause = {0, 5 * 1000 * 1000};
            nanosleep(&pause, NULL);
        }
    }
    assert(accepted >= 0);
    assert(fcntl(accepted, F_GETFL) & O_NONBLOCK);
    nodelay = 0;
    optlen = sizeof(nodelay);
    assert(getsockopt(accepted, IPPROTO_TCP, TCP_NODELAY, &nodelay, &optlen) == 0);
    assert(nodelay != 0);
#if defined(__linux__)
    assert(fcntl(accepted, F_GETFD) & FD_CLOEXEC);
#endif
    char byte;
    assert(read(accepted, &byte, 1) < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)); /* never blocks */
    close(accepted);
    close(client);

    /* Drained: EAGAIN, not a block (the listener itself is non-blocking). */
    assert(accept_client(app.server_fd) < 0);
    assert(errno == EAGAIN || errno == EWOULDBLOCK);

    /* Same options on a connection admitted through the real accept_connections path. */
    const int client2 = connect_loopback_client(&app);
    drain_accept_connections(&app);
    assert(app_count_connections(&app) == 1);
    for (int fd = 0; fd < app.connections_cap; fd++) {
        if (app.connections[fd] != NULL) {
            assert(fcntl(fd, F_GETFL) & O_NONBLOCK);
            nodelay = 0;
            optlen = sizeof(nodelay);
            assert(getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, &optlen) == 0);
            assert(nodelay != 0);
        }
    }
    close(client2);
    app_destroy(&app);
}

/* accept_connections used to accept without limit, bounded only by RLIMIT_NOFILE - a flood of
 * connections had no graceful degradation, just an eventual, silent EMFILE. max_connections caps
 * concurrently open connections per worker; past it, accept_connections still accept()s (it has to,
 * to answer at all) but sends 503 + Connection: close and closes immediately, without registering a
 * Connection or touching the event loop. */
static void test_accept_connections_enforces_max_connections(void) {
    App app;
    setup_test_server(&app);
    app.config.max_connections = 2;

    int clients[3];
    for (int i = 0; i < 3; i++) {
        clients[i] = connect_loopback_client(&app);
    }

    drain_accept_connections(&app);

    assert(app_count_connections(&app) == 2);
    assert(app.open_connections == 2);

    /* The third client gets an explicit 503, not a silent drop or a hung connection. */
    char resp[256];
    memset(resp, 0, sizeof(resp));
    ssize_t n = read(clients[2], resp, sizeof(resp) - 1);
    assert(n > 0);
    assert(strstr(resp, "HTTP/1.1 503 Service Unavailable") != NULL);
    assert(strstr(resp, "Connection: close") != NULL);
    assert(strstr(resp, "\r\nDate: ") != NULL && strstr(resp, " GMT\r\n") != NULL); /* Date header */

    for (int i = 0; i < 3; i++) {
        close(clients[i]);
    }
    app_destroy(&app);
}

/* max_connections == 0 is a deliberate opt-out (uncapped, the old default): the accept path must
 * not treat "unset" as "zero capacity". */
static void test_accept_connections_max_connections_zero_is_unlimited(void) {
    App app;
    setup_test_server(&app);
    app.config.max_connections = 0;

    int clients[5];
    for (int i = 0; i < 5; i++) {
        clients[i] = connect_loopback_client(&app);
    }

    drain_accept_connections(&app);

    assert(app_count_connections(&app) == 5);
    assert(app.open_connections == 5);

    for (int i = 0; i < 5; i++) {
        close(clients[i]);
    }
    app_destroy(&app);
}

/*
 * EMFILE recovery. Simulates the process being genuinely out of file descriptors (not just past
 * the configured max_connections) by lowering RLIMIT_NOFILE to exactly the number of descriptors
 * currently in use, so the very next fd allocation - accept()'s own - is guaranteed to fail with
 * EMFILE.
 *
 * MEASURED on this machine (macOS): accept() does not leave the completed connection in the listen
 * backlog for a retry when it fails this way - it dequeues and destroys it before failing to
 * allocate an fd, so the connecting client sees its connection closed (observed: a clean EOF, no
 * data) rather than an HTTP 503. A prior version of this fix tried to recover that same connection
 * (close the spare fd, retry accept(), answer 503) and found that retry reliably returns EAGAIN
 * instead - proof the connection was already gone, not evidence of a bug in the retry. So this test
 * does not assert a 503 for the connection that triggered EMFILE; it asserts what the fix actually
 * delivers: freeing the spare fd lets the *next* accept() succeed (accept_connections is not
 * permanently stuck at the limit), and connection_close opportunistically re-arms the spare fd once
 * anything closes.
 */
static void test_accept_connections_emfile_frees_a_slot_and_recovers(void) {
    App app;
    setup_test_server(&app);
    assert(app.spare_fd >= 0); /* reserved by app_init; the mechanism under test needs it armed */

    int doomed_client = connect_loopback_client(&app);

    struct rlimit original;
    assert(getrlimit(RLIMIT_NOFILE, &original) == 0);

    /* probe's fd number equals the count of descriptors already in use (fds are handed out
     * lowest-first): pin the limit there so the very next fd allocation fails with EMFILE, without
     * guessing at an absolute descriptor count that varies by environment. */
    int probe = open("/dev/null", O_RDONLY);
    assert(probe >= 0);
    close(probe);
    struct rlimit tight = original;
    tight.rlim_cur = (rlim_t)probe;
    assert(setrlimit(RLIMIT_NOFILE, &tight) == 0);

    drain_accept_connections(&app); /* hits EMFILE: spare_fd is sacrificed, doomed_client's connection is lost */

    assert(setrlimit(RLIMIT_NOFILE, &original) == 0); /* restore before anything else can fail the test */

    assert(app_count_connections(&app) == 0);
    assert(app.spare_fd < 0); /* consumed, not yet re-armed - nothing has closed to trigger that */

    char resp[16];
    ssize_t n = read(doomed_client, resp, sizeof(resp));
    assert(n <= 0); /* closed, not a live connection waiting for a request */
    close(doomed_client);

    /* The real point of the fix: a *later* connection, once descriptors are no longer scarce, is
     * accepted normally - the worker recovered instead of staying stuck refusing everything. */
    int later_client = connect_loopback_client(&app);
    drain_accept_connections(&app);
    assert(app_count_connections(&app) == 1);

    /* Closing it frees a descriptor; connection_close should opportunistically re-arm the spare. */
    Connection *conn = NULL;
    for (int fd = 0; fd < app.connections_cap; fd++) {
        if (app.connections[fd] != NULL) {
            conn = app.connections[fd];
            break;
        }
    }
    assert(conn != NULL);
    connection_close(&app, conn);
    assert(app.spare_fd >= 0);

    close(later_client);
    app_destroy(&app);
}

int main(void) {
    test_set_nonblocking_and_create();
    test_handle_readable_round_trip_success();
    test_handle_readable_keep_alive_multiple_requests();
    test_handle_readable_partial_read();
    test_handle_readable_malformed_400();
    test_handle_readable_malformed_request_line_400();
    test_handle_readable_unmatched_route_404();
    test_handle_readable_header_overflow_431();
    test_handle_readable_long_header_value_is_not_capped();
    test_handle_readable_embedded_nul_in_path_400();
    test_handle_readable_host_header_count_400();
    test_handle_readable_large_body_grows_buffer();
    test_handle_readable_body_too_large_413();
    test_handle_readable_content_length_grows_geometrically();
    test_handle_readable_route_body_limit_413();
    test_handle_readable_route_body_limit_allows_within_limit();
    test_handle_readable_expect_continue_sends_100_once();
    test_handle_readable_expect_continue_chunked();
    test_handle_readable_expect_continue_not_sent();
    test_handle_readable_chunked_round_trip();
    test_handle_readable_head_scan_resumes_and_resets();
    test_handle_readable_chunked_scan_resumes_and_resets();
    test_handle_readable_chunked_grows_buffer();
    test_handle_readable_chunked_too_large_413();
    test_handle_readable_chunked_malformed_400();
    test_handle_readable_chunked_and_content_length_400();
    test_handle_readable_bare_lf_400();
    test_handle_readable_unsupported_transfer_encoding_501();
    test_handle_readable_path_too_long_414();
    test_handle_readable_connection_close();
    test_close_idle_connections_closes_stale_keep_alive();
    test_close_idle_connections_408s_stalled_partial_request();
    test_close_idle_connections_leaves_recent_activity_alone();
    test_close_idle_connections_skips_pending_write();
    test_close_idle_connections_write_stall_closes_connection();
    test_flush_connection_write_stall_reclaimed_by_close_idle_connections();
    test_close_idle_connections_header_deadline_closes_slow_drip();
    test_close_idle_connections_header_deadline_leaves_fresh_partial_request_alone();
    test_close_idle_connections_body_deadline_allows_slow_body_within_window();
    test_close_idle_connections_body_deadline_closes_stalled_body();
    test_handle_readable_head_request_omits_body();
    test_handle_readable_auto_options_response();
    test_app_count_connections();
    test_app_stop_idempotency_and_closing_idle();
    test_handle_readable_during_shutdown_forces_connection_close();
    test_app_stop_drains_and_flushes_pending_write();
    test_res_send_file_streams_to_socket();
    test_flush_connection_file_stream_survives_another_connections_dispatch();
    test_accept_client_socket_options();
    test_accept_connections_enforces_max_connections();
    test_accept_connections_max_connections_zero_is_unlimited();
    test_accept_connections_emfile_frees_a_slot_and_recovers();

    printf("all connection tests passed\n");
    return 0;
}
