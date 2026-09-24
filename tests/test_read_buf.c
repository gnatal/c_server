/*
 * input memory is borrowed per read from the worker's shared App.read_buf, not owned by every
 * connection. An idle connection holds no input buffer; a complete request is parsed in place in
 * App.read_buf; only unserved bytes (a partial request, or requests pipelined behind a pending
 * response) are copied into a connection-owned buffer, which is freed once nothing is buffered.
 * Same socketpair(2) harness as test_pipelining.c; the interleaving tests drive two connections on
 * one App so the second one's read really overwrites App.read_buf between the first one's turns.
 */
#include <assert.h>
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

#define BIG_BODY_LEN (1024 * 1024) /* far larger than a socketpair's kernel buffer: forces EAGAIN */

/* Echoes the path so a response shows which request it answers. */
static void echo_path_handler(const Request *req, Response *res) {
    res_status(res, 200);
    res_send(res, req->path);
}

/* Echoes the X-Tag header and body length: both are read from in_buf (header views). */
static void echo_upload_handler(const Request *req, Response *res) {
    const char *tag = req_get_header(req, "X-Tag");
    char body[128];
    snprintf(body, sizeof(body), "tag=%s len=%d first=%c last=%c", tag != NULL ? tag : "(none)",
             req->content_length, req->content_length > 0 ? req->body[0] : '-',
             req->content_length > 0 ? req->body[req->content_length - 1] : '-');
    res_status(res, 200);
    res_send(res, body);
}

static void big_handler(const Request *req, Response *res) {
    (void)req;
    unsigned char *body = malloc(BIG_BODY_LEN);
    assert(body != NULL);
    memset(body, 'x', BIG_BODY_LEN);
    res_status(res, 200);
    res_send_bytes(res, "application/octet-stream", body, BIG_BODY_LEN);
    free(body); /* res_send_bytes copied it into the arena */
}

/* reports whether req->body is a view into the connection's input buffer (not an arena copy). */
static void body_location_handler(const Request *req, Response *res) {
    const Connection *c = res->conn;
    const int in_input = req->body >= c->in_buf && req->body + req->content_length < c->in_buf + c->in_cap;
    char body[128];
    snprintf(body, sizeof(body), "in_input=%d len=%d nul=%d first=%c", in_input, req->content_length,
             req->body[req->content_length] == '\0', req->content_length > 0 ? req->body[0] : '-');
    res_status(res, 200);
    res_send(res, body);
}

static void register_routes(App *app) {
    app_post(app, "/where", body_location_handler);
    app_get(app, "/a", echo_path_handler);
    app_get(app, "/b", echo_path_handler);
    app_get(app, "/big", big_handler);
    app_post(app, "/upload", echo_upload_handler);
}

/* Adds one socketpair-backed connection to an already initialized App. */
static Connection *add_connection(App *app, int fds[2]) {
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    assert(set_nonblocking(fds[0]) == 0);
    assert(set_nonblocking(fds[1]) == 0);
    /* Client-to-server direction only: room for one write larger than BUF_SIZE (the default
     * socketpair buffer is about 8 KiB on macOS). The server-to-client direction keeps its small
     * default so a 1 MiB response still hits EAGAIN. */
    const int sndbuf = 256 * 1024;
    assert(setsockopt(fds[1], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) == 0);
    Connection *conn = connection_create(app, fds[0]);
    assert(conn != NULL);
    app->connections[fds[0]] = conn;
    assert(event_loop_watch_read(app, fds[0], conn) == 0);
    return conn;
}

static void setup_app(App *app) {
    app_init(app);
    assert(event_loop_init(app) == 0);
    register_routes(app);
}

static void send_bytes(const int fd, const char *data, const size_t len) {
    assert(write(fd, data, len) == (ssize_t)len);
}

static void send_all(const int fd, const char *data) {
    send_bytes(fd, data, strlen(data));
}

/* Everything the server has written so far (non-blocking peer), NUL-terminated. */
static size_t read_available(const int fd, char *out, const size_t cap) {
    size_t total = 0;
    for (;;) {
        const ssize_t n = read(fd, out + total, cap - 1 - total);
        if (n <= 0) {
            break;
        }
        total += (size_t)n;
        if (total == cap - 1) {
            break;
        }
    }
    out[total] = '\0';
    return total;
}

/* A fresh connection, and one between keep-alive requests, owns no input buffer; a complete request
 * is served straight out of App.read_buf. */
static void test_idle_connection_owns_no_input_buffer(void) {
    App app;
    setup_app(&app);
    int fds[2];
    Connection *conn = add_connection(&app, fds);
    assert(app.read_buf != NULL);
    assert(conn->in_buf == NULL && conn->in_cap == 0);

    for (int round = 0; round < 3; round++) {
        send_all(fds[1], "GET /a HTTP/1.1\r\nHost: x\r\n\r\n");
        handle_readable(&app, conn);
        char out[1024];
        read_available(fds[1], out, sizeof(out));
        assert(strstr(out, "HTTP/1.1 200 OK") != NULL && strstr(out, "\r\n\r\n/a") != NULL);
        assert(app.connections[fds[0]] == conn);
        assert(conn->in_buf == NULL && conn->in_cap == 0 && conn->in_len == 0 && conn->in_off == 0);
    }

    close(fds[1]);
    app_destroy(&app);
}

/* The core shared-receive-buffer hazard: connection A's partial request must survive connection B being served out of
 * the same App.read_buf in between. A's header (a view into in_buf) and body bytes are checked
 * in its eventual response. */
static void test_partial_request_survives_another_connections_read(void) {
    App app;
    setup_app(&app);
    int fds_a[2];
    int fds_b[2];
    Connection *a = add_connection(&app, fds_a);
    Connection *b = add_connection(&app, fds_b);

    const char *a_head = "POST /upload HTTP/1.1\r\nHost: x\r\nX-Tag: from-a\r\nContent-Length: 6\r\n\r\nAAA";
    send_all(fds_a[1], a_head);
    handle_readable(&app, a);
    assert(a->in_buf != NULL && a->in_buf != app.read_buf); /* copied off the shared buffer */
    assert(a->in_cap == BUF_SIZE);
    assert(a->in_off == 0 && a->in_len == strlen(a_head));
    assert(memcmp(a->in_buf, a_head, a->in_len) == 0 && a->in_buf[a->in_len] == '\0');
    assert(a->request_started != 0);

    /* B's whole request lands in App.read_buf, overwriting whatever A's read left there. */
    send_all(fds_b[1], "POST /upload HTTP/1.1\r\nHost: x\r\nX-Tag: from-b\r\nContent-Length: 3\r\n\r\nBBB");
    handle_readable(&app, b);
    char out[1024];
    read_available(fds_b[1], out, sizeof(out));
    assert(strstr(out, "tag=from-b len=3 first=B last=B") != NULL);
    assert(b->in_buf == NULL);

    send_all(fds_a[1], "AAZ");
    handle_readable(&app, a);
    read_available(fds_a[1], out, sizeof(out));
    assert(strstr(out, "tag=from-a len=6 first=A last=Z") != NULL);
    assert(a->in_buf == NULL && a->in_cap == 0); /* owned buffer freed once served */

    close(fds_a[1]);
    close(fds_b[1]);
    app_destroy(&app);
}

/* A body that doesn't fit BUF_SIZE starts in App.read_buf and must move to an owned buffer when
 * grown (never realloc the shared one), with every byte read so far carried over. */
static void test_growth_from_read_buf_moves_to_owned_buffer(void) {
    App app;
    setup_app(&app);
    int fds[2];
    Connection *conn = add_connection(&app, fds);

    const size_t body_len = 20000;
    char *wire = malloc(256 + body_len);
    assert(wire != NULL);
    const int head_len = snprintf(wire, 256, "POST /upload HTTP/1.1\r\nHost: x\r\nX-Tag: big\r\nContent-Length: %zu\r\n\r\n",
                                  body_len);
    assert(head_len > 0);
    memset(wire + head_len, 'y', body_len);
    wire[(size_t)head_len + body_len - 1] = 'Q';
    const size_t wire_len = (size_t)head_len + body_len;

    /* One write larger than BUF_SIZE: the first recv fills App.read_buf to BUF_SIZE - 1. */
    send_bytes(fds[1], wire, BUF_SIZE + 100);
    handle_readable(&app, conn);
    assert(conn->in_buf != NULL && conn->in_buf != app.read_buf);
    assert(conn->in_cap > BUF_SIZE);
    assert(conn->in_len == BUF_SIZE - 1);
    assert(memcmp(conn->in_buf, wire, conn->in_len) == 0);

    size_t sent = BUF_SIZE + 100;
    char out[1024] = {0};
    for (int spins = 0; spins < 1000 && strstr(out, "HTTP/1.1") == NULL; spins++) {
        if (sent < wire_len) {
            const size_t piece = wire_len - sent < 4096 ? wire_len - sent : 4096;
            send_bytes(fds[1], wire + sent, piece);
            sent += piece;
        }
        handle_readable(&app, conn);
        read_available(fds[1], out, sizeof(out));
    }
    assert(strstr(out, "tag=big len=20000 first=y last=Q") != NULL);
    assert(conn->in_buf == NULL && conn->in_cap == 0);

    free(wire);
    close(fds[1]);
    app_destroy(&app);
}

/* A request pipelined behind a response that hits EAGAIN is copied off App.read_buf; another
 * connection then uses the shared buffer before the pending response drains, and the pipelined
 * request is still answered correctly afterwards. */
static void test_pipelined_leftover_behind_pending_response_is_owned(void) {
    App app;
    setup_app(&app);
    int fds[2];
    int fds_other[2];
    Connection *conn = add_connection(&app, fds);
    Connection *other = add_connection(&app, fds_other);

    const char *follow_up = "GET /b HTTP/1.1\r\nHost: x\r\n\r\n";
    send_all(fds[1], "GET /big HTTP/1.1\r\nHost: x\r\n\r\n");
    send_all(fds[1], follow_up);
    handle_readable(&app, conn);
    assert(conn->out_buf != NULL && conn->out_sent < conn->out_len); /* real EAGAIN mid-response */
    assert(conn->in_buf != NULL && conn->in_buf != app.read_buf);
    assert(conn->in_off == 0); /* compacted: the dispatched /big request is at 0, request_len long */
    assert(conn->in_len == conn->request_len + strlen(follow_up));

    send_all(fds_other[1], "GET /a HTTP/1.1\r\nHost: x\r\n\r\n");
    handle_readable(&app, other);
    char small[1024];
    read_available(fds_other[1], small, sizeof(small));
    assert(strstr(small, "\r\n\r\n/a") != NULL);

    static char out[BIG_BODY_LEN + 8192];
    size_t got = 0;
    for (int spins = 0; spins < 100000; spins++) {
        const ssize_t n = read(fds[1], out + got, sizeof(out) - 1 - got);
        if (n > 0) {
            got += (size_t)n;
        }
        if (conn->out_buf == NULL && conn->file_fd < 0 && conn->in_len == 0) {
            break;
        }
        handle_writable(&app, conn);
    }
    for (ssize_t n; (n = read(fds[1], out + got, sizeof(out) - 1 - got)) > 0;) {
        got += (size_t)n;
    }
    out[got] = '\0';
    assert(got > BIG_BODY_LEN);
    assert(strstr(out + BIG_BODY_LEN, "\r\n\r\n/b") != NULL);
    assert(conn->in_buf == NULL && conn->in_cap == 0);

    close(fds[1]);
    close(fds_other[1]);
    app_destroy(&app);
}

/* A rejection while reading into App.read_buf closes the connection without freeing the shared
 * buffer: the next connection still reads through it (an ASan run catches a free of it). */
static void test_rejection_from_read_buf_keeps_shared_buffer(void) {
    App app;
    setup_app(&app);
    int fds_bad[2];
    int fds_good[2];
    Connection *bad = add_connection(&app, fds_bad);
    Connection *good = add_connection(&app, fds_good);

    send_all(fds_bad[1], "NOT A REQUEST\r\n\r\n");
    handle_readable(&app, bad);
    assert(app.connections[fds_bad[0]] == NULL);
    char out[1024];
    read_available(fds_bad[1], out, sizeof(out));
    assert(strstr(out, "400") != NULL);

    /* Headers over BUF_SIZE with no terminator: 431 straight from the borrowed buffer, as before the shared receive buffer. */
    int fds_huge[2];
    Connection *huge = add_connection(&app, fds_huge);
    char big_headers[BUF_SIZE + 64];
    const int n = snprintf(big_headers, sizeof(big_headers), "GET /a HTTP/1.1\r\nHost: x\r\nX-Pad: ");
    memset(big_headers + n, 'p', sizeof(big_headers) - (size_t)n);
    send_bytes(fds_huge[1], big_headers, sizeof(big_headers));
    handle_readable(&app, huge);
    assert(app.connections[fds_huge[0]] == NULL);
    read_available(fds_huge[1], out, sizeof(out));
    assert(strstr(out, "431") != NULL);

    send_all(fds_good[1], "GET /a HTTP/1.1\r\nHost: x\r\n\r\n");
    handle_readable(&app, good);
    read_available(fds_good[1], out, sizeof(out));
    assert(strstr(out, "\r\n\r\n/a") != NULL);

    close(fds_bad[1]);
    close(fds_huge[1]);
    close(fds_good[1]);
    app_destroy(&app);
}

/* close_idle_connections' 408 for a half-received request reads the connection's owned buffer (the
 * shared one has long since been reused), and closing it frees that buffer. */
static void test_idle_sweep_408_with_owned_partial_buffer(void) {
    App app;
    setup_app(&app);
    int fds[2];
    Connection *conn = add_connection(&app, fds);

    send_all(fds[1], "GET /a HTTP/1.1\r\nHo");
    handle_readable(&app, conn);
    assert(conn->in_buf != NULL && conn->in_buf != app.read_buf);
    conn->request_started -= REQUEST_HEADER_TIMEOUT_SECONDS + 1;
    close_idle_connections(&app);
    assert(app.connections[fds[0]] == NULL);
    char out[1024];
    read_available(fds[1], out, sizeof(out));
    assert(strstr(out, "408") != NULL);

    close(fds[1]);
    app_destroy(&app);
}

/* the handler's req->body is a view into in_buf - the borrowed App.read_buf for a small body, the
 * grown owned buffer for a large one, decoded in place for chunked - never an arena copy. A request
 * pipelined right behind a Content-Length body (its first byte held the body's NUL) is still served. */
static void test_body_is_a_view_into_the_input_buffer(void) {
    App app;
    setup_app(&app);
    int fds[2];
    Connection *conn = add_connection(&app, fds);
    char out[4096];

    send_all(fds[1], "POST /where HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhelloGET /a HTTP/1.1\r\nHost: x\r\n\r\n");
    handle_readable(&app, conn);
    read_available(fds[1], out, sizeof(out));
    assert(strstr(out, "in_input=1 len=5 nul=1 first=h") != NULL);
    assert(strstr(out, "\r\n\r\n/a") != NULL); /* 'G' restored after the body's NUL */

    send_all(fds[1], "POST /where HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n"
                     "GET /b HTTP/1.1\r\nHost: x\r\n\r\n");
    handle_readable(&app, conn);
    read_available(fds[1], out, sizeof(out));
    assert(strstr(out, "in_input=1 len=5 nul=1 first=a") != NULL);
    assert(strstr(out, "\r\n\r\n/b") != NULL);

    const size_t body_len = 20000;
    char *wire = malloc(256 + body_len);
    assert(wire != NULL);
    const int head_len = snprintf(wire, 256, "POST /where HTTP/1.1\r\nHost: x\r\nContent-Length: %zu\r\n\r\n", body_len);
    memset(wire + head_len, 'z', body_len);
    const size_t wire_len = (size_t)head_len + body_len;
    size_t sent = 0;
    out[0] = '\0';
    for (int spins = 0; spins < 1000 && strstr(out, "HTTP/1.1") == NULL; spins++) {
        if (sent < wire_len) {
            const size_t piece = wire_len - sent < 4096 ? wire_len - sent : 4096;
            send_bytes(fds[1], wire + sent, piece);
            sent += piece;
        }
        handle_readable(&app, conn);
        read_available(fds[1], out, sizeof(out));
    }
    assert(strstr(out, "in_input=1 len=20000 nul=1 first=z") != NULL);
    free(wire);

    close(fds[1]);
    app_destroy(&app);
}

int main(void) {
    test_idle_connection_owns_no_input_buffer();
    test_partial_request_survives_another_connections_read();
    test_growth_from_read_buf_moves_to_owned_buffer();
    test_pipelined_leftover_behind_pending_response_is_owned();
    test_rejection_from_read_buf_keeps_shared_buffer();
    test_idle_sweep_408_with_owned_partial_buffer();
    test_body_is_a_view_into_the_input_buffer();
    printf("all read_buf tests passed\n");
    return 0;
}
