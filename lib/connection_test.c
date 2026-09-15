#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/event.h>
#include "appTypes.h"
#include "connection.h"
#include "router.h"
#include "response.h"
#include "middleware.h"

static void ping_handler(const Request *req, Response *res) {
    (void)req;
    res_status(res, 200);
    res_send(res, "pong");
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
    if (app->connections[fds[0]] != NULL) {
        connection_close(app, conn);
    }
    close(fds[1]);
    if (app->kq >= 0) {
        close(app->kq);
        app->kq = -1;
    }
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
    assert(c->out_buf == NULL);
    assert(c->in_len == 0);
    assert(c->out_len == 0);

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

int main(void) {
    test_set_nonblocking_and_create();
    test_handle_readable_round_trip_success();
    test_handle_readable_keep_alive_multiple_requests();
    test_handle_readable_partial_read();
    test_handle_readable_malformed_400();
    test_handle_readable_unmatched_route_404();
    test_handle_readable_header_overflow_431();
    test_handle_readable_connection_close();

    printf("all connection tests passed\n");
    return 0;
}
