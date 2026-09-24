#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include "app_types.h"
#include "event_loop.h"
#include "connection.h"
#include "router.h"
#include "response.h"
#include "http_parser.h"

/*
 * ServerConfig.max_buffered_bytes / App.buffered_bytes: the per-worker budget for memory connections
 * hold across event-loop turns (owned in_bufs, owned out_buf tail copies, stream_bufs). Driven through
 * the real handle_readable / flush_connection path over socketpair(2).
 */

static void echo_len_handler(const Request *req, Response *res) {
    char body[64];
    snprintf(body, sizeof(body), "received %d bytes", req->content_length);
    res_send(res, body);
}

#define BIG_RESPONSE_LEN (1024 * 1024)
static char g_big[BIG_RESPONSE_LEN + 1];

static void big_handler(const Request *req, Response *res) {
    (void)req;
    res_send(res, g_big);
}

static int pausing_producer(StreamWriter *w, void *ctx) {
    (void)w;
    (void)ctx;
    return STREAM_PAUSE;
}

static void stream_handler(const Request *req, Response *res) {
    (void)req;
    assert(res_stream(res, pausing_producer, NULL, NULL) == 0);
}

static void app_setup(App *app) {
    app_init(app);
    assert(event_loop_init(app) == 0);
    app_post(app, "/upload", echo_len_handler);
    app_get(app, "/big", big_handler);
    app_get(app, "/events", stream_handler);
}

/* A Connection on one end of a fresh socketpair; the peer end goes to *peer. */
static Connection *open_conn(App *app, int *peer) {
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    assert(set_nonblocking(fds[0]) == 0);
    assert(set_nonblocking(fds[1]) == 0);
    Connection *conn = connection_create(app, fds[0]);
    assert(conn != NULL);
    assert(fds[0] < app->connections_cap); /* app_init sizes the table well past a test's fds */
    app->connections[fds[0]] = conn;
    app->open_connections++;
    assert(event_loop_watch_read(app, fds[0], conn) == 0);
    *peer = fds[1];
    return conn;
}

static void send_all(const int fd, const char *data, const size_t len) {
    assert(write(fd, data, len) == (ssize_t)len);
}

/* Reads what the server has written so far (non-blocking), NUL-terminated. */
static size_t read_some(const int fd, char *out, const size_t cap) {
    size_t total = 0;
    while (total + 1 < cap) {
        const ssize_t n = read(fd, out + total, cap - 1 - total);
        if (n <= 0) {
            break;
        }
        total += (size_t)n;
    }
    out[total] = '\0';
    return total;
}

static void test_app_init_sets_default_budget(void) {
    App app;
    memset(&app, 0xAB, sizeof(app));
    app_init(&app);
    assert(app.config.max_buffered_bytes == DEFAULT_MAX_BUFFERED_BYTES);
    assert(app.buffered_bytes == 0);
    app_destroy(&app);
}

/* An upload is counted while it grows and released once answered; nothing is left counted after. */
static void test_upload_is_counted_and_released(void) {
    App app;
    app_setup(&app);
    int peer;
    const int fd = (open_conn(&app, &peer))->fd;
    Connection *conn = app.connections[fd];

    const size_t body_len = 40000;
    char head[128];
    snprintf(head, sizeof(head), "POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: %zu\r\n\r\n", body_len);
    send_all(peer, head, strlen(head));
    handle_readable(&app, conn);
    /* partial request moved off App.read_buf into an owned BUF_SIZE buffer */
    assert(conn->held_bytes == BUF_SIZE && app.buffered_bytes == BUF_SIZE);

    char chunk[4096];
    memset(chunk, 'x', sizeof(chunk));
    size_t sent = 0;
    size_t peak = 0;
    while (sent < body_len) {
        const size_t n = body_len - sent < sizeof(chunk) ? body_len - sent : sizeof(chunk);
        send_all(peer, chunk, n);
        sent += n;
        handle_readable(&app, conn);
        if (app.connections[fd] == conn && sent < body_len) {
            assert(app.buffered_bytes == conn->held_bytes && conn->held_bytes == conn->in_cap);
            if (app.buffered_bytes > peak) {
                peak = app.buffered_bytes;
            }
        }
    }
    assert(peak > BUF_SIZE);

    char resp[256];
    read_some(peer, resp, sizeof(resp));
    assert(strstr(resp, "HTTP/1.1 200 OK") != NULL && strstr(resp, "received 40000 bytes") != NULL);
    assert(app.connections[fd] == conn);
    assert(conn->in_buf == NULL && conn->held_bytes == 0 && app.buffered_bytes == 0);

    close(peer);
    app_destroy(&app);
}

/* Growth past the budget answers 503 and closes; the connection's share is released. */
static void test_upload_over_budget_503(void) {
    App app;
    app_setup(&app);
    app.config.max_buffered_bytes = 20000;
    int peer;
    const int fd = (open_conn(&app, &peer))->fd;

    char head[128];
    snprintf(head, sizeof(head), "POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n", 100000);
    send_all(peer, head, strlen(head));
    handle_readable(&app, app.connections[fd]);

    char chunk[4096];
    memset(chunk, 'x', sizeof(chunk));
    for (int i = 0; i < 24 && app.connections[fd] != NULL; i++) {
        send_all(peer, chunk, sizeof(chunk));
        handle_readable(&app, app.connections[fd]);
        assert(app.buffered_bytes <= app.config.max_buffered_bytes);
    }
    assert(app.connections[fd] == NULL);
    assert(app.buffered_bytes == 0);
    char resp[256];
    read_some(peer, resp, sizeof(resp));
    assert(strncmp(resp, "HTTP/1.1 503 ", 13) == 0);
    assert(strstr(resp, "Connection: close\r\n") != NULL);

    /* the same upload with no budget (0) is served */
    app.config.max_buffered_bytes = 0;
    int peer2;
    Connection *conn2 = open_conn(&app, &peer2);
    send_all(peer2, head, strlen(head));
    handle_readable(&app, conn2);
    char *body = malloc(100000);
    memset(body, 'y', 100000);
    size_t sent = 0;
    while (sent < 100000) {
        const size_t n = 100000 - sent < 4096 ? 100000 - sent : 4096;
        send_all(peer2, body + sent, n);
        sent += n;
        handle_readable(&app, conn2);
    }
    read_some(peer2, resp, sizeof(resp));
    assert(strstr(resp, "received 100000 bytes") != NULL);
    assert(app.buffered_bytes == 0);

    free(body);
    close(peer);
    close(peer2);
    app_destroy(&app);
}

/* A partial request that would take the worker over budget is answered 503; others are untouched. */
static void test_partial_request_over_budget_503(void) {
    App app;
    app_setup(&app);
    app.config.max_buffered_bytes = BUF_SIZE;
    const char *partial = "POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nabc";

    int peer_a;
    Connection *a = open_conn(&app, &peer_a);
    send_all(peer_a, partial, strlen(partial));
    handle_readable(&app, a);
    assert(a->held_bytes == BUF_SIZE && app.buffered_bytes == BUF_SIZE);

    int peer_b;
    const int fd_b = (open_conn(&app, &peer_b))->fd;
    send_all(peer_b, partial, strlen(partial));
    handle_readable(&app, app.connections[fd_b]);
    assert(app.connections[fd_b] == NULL);
    char resp[256];
    read_some(peer_b, resp, sizeof(resp));
    assert(strncmp(resp, "HTTP/1.1 503 ", 13) == 0);
    assert(app.buffered_bytes == BUF_SIZE);

    /* A completes normally; a whole request that needs no owned buffer is never refused */
    send_all(peer_a, "defghij", 7);
    handle_readable(&app, a);
    read_some(peer_a, resp, sizeof(resp));
    assert(strstr(resp, "received 10 bytes") != NULL);
    assert(app.buffered_bytes == 0);

    close(peer_a);
    close(peer_b);
    app_destroy(&app);
}

/* A response the client does not read: its tail copy is counted, and over budget the connection is closed. */
static void test_unread_response_tail(void) {
    memset(g_big, 'b', BIG_RESPONSE_LEN);
    g_big[BIG_RESPONSE_LEN] = '\0';
    const char *get = "GET /big HTTP/1.1\r\nHost: x\r\n\r\n";

    App app;
    app_setup(&app);
    int peer;
    const int fd = (open_conn(&app, &peer))->fd;
    Connection *conn = app.connections[fd];
    send_all(peer, get, strlen(get));
    handle_readable(&app, conn);
    /* nobody reads peer: flush hit EAGAIN and copied the unsent tail */
    assert(app.connections[fd] == conn && conn->out_buf_owned);
    assert(conn->held_bytes == conn->out_cap && app.buffered_bytes == conn->out_cap);
    assert(conn->out_cap > 0 && conn->out_cap < BIG_RESPONSE_LEN + 256);

    /* drain it: the tail is freed and uncounted once fully written */
    char *sink = malloc(BIG_RESPONSE_LEN + 1024);
    size_t got = 0;
    while (app.connections[fd] == conn && conn->out_buf != NULL) {
        got += read_some(peer, sink + got, BIG_RESPONSE_LEN + 1024 - got);
        handle_writable(&app, conn);
    }
    got += read_some(peer, sink + got, BIG_RESPONSE_LEN + 1024 - got);
    assert(got > BIG_RESPONSE_LEN);
    assert(app.connections[fd] == conn && app.buffered_bytes == 0 && conn->held_bytes == 0);

    /* same request with a budget smaller than the tail: the stalled connection is closed, not held */
    app.config.max_buffered_bytes = 64 * 1024;
    send_all(peer, get, strlen(get));
    handle_readable(&app, conn);
    assert(app.connections[fd] == NULL);
    assert(app.buffered_bytes == 0);

    free(sink);
    close(peer);
    app_destroy(&app);
}

/* A parked producer stream keeps its stream_buf, which is counted until the connection closes. */
static void test_stream_buf_counted(void) {
    App app;
    app_setup(&app);
    int peer;
    const int fd = (open_conn(&app, &peer))->fd;
    Connection *conn = app.connections[fd];
    const char *get = "GET /events HTTP/1.1\r\nHost: x\r\n\r\n";
    send_all(peer, get, strlen(get));
    handle_readable(&app, conn);
    if (conn->stream_buf == NULL) {
        handle_writable(&app, conn); /* the producer's first turn */
    }
    assert(conn->stream_fn != NULL && conn->stream_buf != NULL);
    assert(conn->held_bytes == STREAM_CHUNK_SIZE && app.buffered_bytes == STREAM_CHUNK_SIZE);
    connection_close(&app, conn);
    assert(app.buffered_bytes == 0);
    close(peer);
    app_destroy(&app);
}

int main(void) {
    test_app_init_sets_default_budget();
    test_upload_is_counted_and_released();
    test_upload_over_budget_503();
    test_partial_request_over_budget_503();
    test_unread_response_tail();
    test_stream_buf_counted();
    printf("all buffer budget tests passed\n");
    return 0;
}
