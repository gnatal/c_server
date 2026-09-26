/*
 * HTTP/1.1 pipelining - several requests arriving in one buffer are each answered, in order, on
 * the same connection. Split from test_connection.c (already past the 1,000-line cap) but driven the
 * same way: a socketpair(2) peer, handle_readable/handle_writable called directly, no real listener.
 */
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

#define BIG_BODY_LEN (1024 * 1024) /* far larger than a socketpair's kernel buffer: forces EAGAIN */

static void ping_handler(const Request *req, Response *res) {
    (void)req;
    res_status(res, 200);
    res_send(res, "pong");
}

/* Echoes the path so the order of answers is visible in the response stream. */
static void echo_path_handler(const Request *req, Response *res) {
    res_status(res, 200);
    res_send(res, req->path);
}

static void echo_len_handler(const Request *req, Response *res) {
    char body[64];
    snprintf(body, sizeof(body), "received %d bytes", req->content_length);
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

#define KB_BODY_LEN 3000 /* five of these fill most of BATCH_BUF_SIZE, the sixth does not fit */

static void kb_handler(const Request *req, Response *res) {
    (void)req;
    char body[KB_BODY_LEN + 1];
    memset(body, 'k', KB_BODY_LEN);
    body[KB_BODY_LEN] = '\0';
    res_status(res, 200);
    res_send(res, body);
}

static SharedBody *shared_hello; /* set by setup, released by teardown */

static void shared_handler(const Request *req, Response *res) {
    (void)req;
    res_status(res, 200);
    res_send_shared(res, "text/plain", shared_hello);
}

/* type: SOCK_STREAM, or SOCK_DGRAM to count the server's write/writev calls (one datagram each). */
static void setup_type(App *app, int fds[2], Connection **conn, const int type) {
    app_init(app);
    assert(event_loop_init(app) == 0);
    assert(socketpair(AF_UNIX, type, 0, fds) == 0);
    if (type == SOCK_DGRAM) {
        const int big = 256 * 1024; /* a datagram is bounded by the sender's SO_SNDBUF */
        assert(setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &big, sizeof(big)) == 0);
        assert(setsockopt(fds[1], SOL_SOCKET, SO_RCVBUF, &big, sizeof(big)) == 0);
    }
    assert(set_nonblocking(fds[0]) == 0);
    assert(set_nonblocking(fds[1]) == 0);
    *conn = connection_create(app, fds[0]);
    assert(*conn != NULL);
    app->connections[fds[0]] = *conn;
    assert(event_loop_watch_read(app, fds[0], *conn) == 0);

    app_get(app, "/ping", ping_handler);
    app_get(app, "/a", echo_path_handler);
    app_get(app, "/b", echo_path_handler);
    app_get(app, "/c", echo_path_handler);
    app_get(app, "/big", big_handler);
    app_get(app, "/kb", kb_handler);
    app_get(app, "/shared", shared_handler);
    app_post(app, "/upload", echo_len_handler);

    shared_hello = shared_body_new(5);
    assert(shared_hello != NULL);
    memcpy(shared_hello->data, "hello", 5);
}

static void setup(App *app, int fds[2], Connection **conn) {
    setup_type(app, fds, conn, SOCK_STREAM);
}

static void teardown(App *app, int fds[2]) {
    close(fds[1]);
    app_destroy(app); /* closes and frees whatever connection is still tracked */
    shared_body_release(shared_hello);
    shared_hello = NULL;
}

static void send_all(const int fd, const char *data) {
    const size_t len = strlen(data);
    assert(write(fd, data, len) == (ssize_t)len);
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

static int count_occurrences(const char *haystack, const char *needle) {
    int count = 0;
    for (const char *p = strstr(haystack, needle); p != NULL; p = strstr(p + 1, needle)) {
        count++;
    }
    return count;
}

/* Two GETs in one write: the second used to be discarded with in_len = 0 (one answer for two). */
static void test_two_gets_in_one_write_both_answered_in_order(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup(&app, fds, &conn);

    send_all(fds[1], "GET /a HTTP/1.1\r\nHost: x\r\n\r\nGET /b HTTP/1.1\r\nHost: x\r\n\r\n");
    handle_readable(&app, conn);

    char out[4096];
    read_available(fds[1], out, sizeof(out));
    assert(count_occurrences(out, "HTTP/1.1 200 OK") == 2);
    const char *first = strstr(out, "\r\n\r\n/a");
    const char *second = strstr(out, "\r\n\r\n/b");
    assert(first != NULL && second != NULL && first < second);

    assert(app.connections[fds[0]] == conn);
    assert(conn->in_len == 0 && conn->in_off == 0 && conn->request_len == 0);
    assert(conn->request_started == 0); /* nothing left buffered: idle between requests */

    teardown(&app, fds);
}

/* A Content-Length body ends exactly where the next request starts - not at in_len. */
static void test_content_length_body_then_get(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup(&app, fds, &conn);

    send_all(fds[1], "POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello"
                     "GET /ping HTTP/1.1\r\nHost: x\r\n\r\n");
    handle_readable(&app, conn);

    char out[4096];
    read_available(fds[1], out, sizeof(out));
    const char *upload = strstr(out, "received 5 bytes");
    const char *pong = strstr(out, "pong");
    assert(upload != NULL && pong != NULL && upload < pong);
    assert(app.connections[fds[0]] == conn);
    assert(conn->in_len == 0);

    teardown(&app, fds);
}

/* A chunked body (with a trailer) ends after its trailer's blank line (ChunkScanState.body_end). */
static void test_chunked_body_then_get(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup(&app, fds, &conn);

    send_all(fds[1], "POST /upload HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
                     "5\r\nHello\r\n6\r\n World\r\n0\r\nX-Trailer: t\r\n\r\n"
                     "GET /ping HTTP/1.1\r\nHost: x\r\n\r\n");
    handle_readable(&app, conn);

    char out[4096];
    read_available(fds[1], out, sizeof(out));
    const char *upload = strstr(out, "received 11 bytes");
    const char *pong = strstr(out, "pong");
    assert(upload != NULL && pong != NULL && upload < pong);
    assert(conn->in_len == 0);
    assert(conn->chunk_scan.pos == 0 && conn->chunk_scan.body_end == 0); /* reset for the next request */

    teardown(&app, fds);
}

/* One full request plus half of the next: the half is kept (moved to the front of in_buf, its request
 * clock started) and completed by the next read. */
static void test_partial_second_request_is_kept_and_completed(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup(&app, fds, &conn);

    const char *half = "GET /b HTTP/1.1\r\nHo";
    char first_write[256];
    snprintf(first_write, sizeof(first_write), "GET /a HTTP/1.1\r\nHost: x\r\n\r\n%s", half);
    send_all(fds[1], first_write);
    handle_readable(&app, conn);

    char out[4096];
    read_available(fds[1], out, sizeof(out));
    assert(count_occurrences(out, "HTTP/1.1 200 OK") == 1);
    assert(strstr(out, "\r\n\r\n/a") != NULL);
    assert(conn->in_off == 0);
    assert(conn->in_len == strlen(half));
    assert(memcmp(conn->in_buf, half, strlen(half)) == 0);
    assert(conn->request_started != 0);

    send_all(fds[1], "st: x\r\n\r\n");
    handle_readable(&app, conn);
    read_available(fds[1], out, sizeof(out));
    assert(count_occurrences(out, "HTTP/1.1 200 OK") == 1);
    assert(strstr(out, "\r\n\r\n/b") != NULL);
    assert(conn->in_len == 0);

    teardown(&app, fds);
}

/* More than MAX_PIPELINED_PER_EVENT requests: the first batch is served, the rest wait behind a
 * write-readiness wakeup (fairness), and handle_writable serves them without any new input. */
static void test_per_event_cap_yields_then_resumes(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup(&app, fds, &conn);

    const int total = MAX_PIPELINED_PER_EVENT + 4;
    char batch[2048];
    size_t off = 0;
    for (int i = 0; i < total; i++) {
        off += (size_t)snprintf(batch + off, sizeof(batch) - off, "GET /ping HTTP/1.1\r\nHost: x\r\n\r\n");
    }
    send_all(fds[1], batch);
    handle_readable(&app, conn);

    char out[16384];
    read_available(fds[1], out, sizeof(out));
    assert(count_occurrences(out, "pong") == MAX_PIPELINED_PER_EVENT);
    /* the 4 unserved requests were moved off the borrowed App.read_buf into an owned buffer,
     * compacted to offset 0, before handle_readable returned. */
    assert(conn->in_buf != NULL && conn->in_buf != app.read_buf);
    assert(conn->in_off == 0 && conn->in_len == 4 * strlen("GET /ping HTTP/1.1\r\nHost: x\r\n\r\n"));
    assert(conn->events_watched & EVENT_WRITE);

    /* The armed write-readiness wakeup: serves the rest with no new input. */
    handle_writable(&app, conn);
    read_available(fds[1], out, sizeof(out));
    assert(count_occurrences(out, "pong") == 4);
    assert(conn->in_len == 0 && conn->in_off == 0);
    assert(conn->in_buf == NULL); /* owned leftover buffer freed once drained */
    assert(!(conn->events_watched & EVENT_WRITE));
    assert(conn->events_watched & EVENT_READ);

    teardown(&app, fds);
}

/* Connection: close on the first request ends the connection after its response; what was
 * pipelined behind it is never answered. */
static void test_connection_close_drops_the_rest(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup(&app, fds, &conn);

    send_all(fds[1], "GET /a HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\nGET /b HTTP/1.1\r\nHost: x\r\n\r\n");
    handle_readable(&app, conn);

    char out[4096];
    read_available(fds[1], out, sizeof(out));
    assert(count_occurrences(out, "HTTP/1.1 200 OK") == 1);
    assert(strstr(out, "/b") == NULL);
    assert(app.connections[fds[0]] == NULL);

    teardown(&app, fds);
}

/* A malformed request after a good one: the good one is answered, then 400 and close. */
static void test_malformed_second_request_gets_400_after_first_answer(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup(&app, fds, &conn);

    send_all(fds[1], "GET /a HTTP/1.1\r\nHost: x\r\n\r\nGARBAGE\r\n\r\n");
    handle_readable(&app, conn);

    char out[4096];
    read_available(fds[1], out, sizeof(out));
    const char *ok = strstr(out, "HTTP/1.1 200 OK");
    const char *bad = strstr(out, "HTTP/1.1 400");
    assert(ok != NULL && bad != NULL && ok < bad);
    assert(app.connections[fds[0]] == NULL);

    teardown(&app, fds);
}

/* A response too large to write in one go leaves requests pipelined behind it untouched: reads stop
 * (no second dispatch over the pending response, which is what happened before pipelining support when a request
 * arrived mid-write), and handle_writable serves them once the big response has fully drained. */
static void test_pending_response_holds_pipeline_until_drained(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup(&app, fds, &conn);

    send_all(fds[1], "GET /big HTTP/1.1\r\nHost: x\r\n\r\nGET /a HTTP/1.1\r\nHost: x\r\n\r\n");
    handle_readable(&app, conn);

    assert(app.connections[fds[0]] == conn);
    assert(conn->out_buf != NULL && conn->out_sent < conn->out_len); /* real EAGAIN mid-response */
    assert(conn->out_buf_owned);
    assert(conn->events_watched & EVENT_WRITE);
    assert(!(conn->events_watched & EVENT_READ));

    /* More input while pending is not consumed. */
    send_all(fds[1], "GET /c HTTP/1.1\r\nHost: x\r\n\r\n");
    const size_t in_len_before = conn->in_len;
    handle_readable(&app, conn);
    assert(conn->in_len == in_len_before);

    /* Drain the peer while the server keeps writing; count body bytes and find the follow-ups. */
    static char out[BIG_BODY_LEN + 8192];
    size_t got = 0;
    for (int spins = 0; spins < 100000 && app.connections[fds[0]] == conn; spins++) {
        const ssize_t n = read(fds[1], out + got, sizeof(out) - 1 - got);
        if (n > 0) {
            got += (size_t)n;
        }
        if (conn->out_buf == NULL && conn->file_fd < 0 && conn->in_len == 0) {
            break;
        }
        handle_writable(&app, conn);
        if (conn->events_watched & EVENT_READ) {
            handle_readable(&app, conn); /* reads resumed: pick up /c */
        }
    }
    for (ssize_t n; (n = read(fds[1], out + got, sizeof(out) - 1 - got)) > 0;) {
        got += (size_t)n;
    }
    out[got] = '\0';

    assert(conn->out_buf == NULL);
    assert(conn->in_len == 0);
    assert(conn->events_watched & EVENT_READ);
    assert(got > BIG_BODY_LEN);
    /* The big body is 'x' bytes, so the follow-up answers are found after it, in order. */
    const char *after_big = out + BIG_BODY_LEN;
    const char *a = strstr(after_big, "\r\n\r\n/a");
    const char *c = strstr(after_big, "\r\n\r\n/c");
    assert(a != NULL && c != NULL && a < c);
    assert(count_occurrences(after_big, "HTTP/1.1 200 OK") == 2);

    teardown(&app, fds);
}

/* ---- coalesced pipelined responses (App.batch_buf) ---- */

/* Reads every datagram the server has sent into out (NUL-terminated); returns how many there were,
 * i.e. how many write/writev calls the server made. */
static int read_datagrams(const int fd, char *out, const size_t cap, size_t *len_out) {
    int count = 0;
    size_t total = 0;
    for (;;) {
        const ssize_t n = recv(fd, out + total, cap - 1 - total, 0);
        if (n <= 0) {
            break;
        }
        total += (size_t)n;
        count++;
    }
    out[total] = '\0';
    if (len_out != NULL) {
        *len_out = total;
    }
    return count;
}

/* Removes every "Date: ...\r\n" line in place, so answers built in different seconds compare equal. */
static void strip_dates(char *s) {
    char *w = s;
    for (const char *r = s; *r != '\0';) {
        if (strncmp(r, "Date: ", 6) == 0 && (r == s || r[-1] == '\n')) {
            const char *eol = strstr(r, "\r\n");
            assert(eol != NULL);
            r = eol + 2;
            continue;
        }
        *w++ = *r++;
    }
    *w = '\0';
}

/* The answers each request gets when sent on its own connection, one at a time, concatenated
 * (Date lines removed): what a pipelined run must reproduce byte for byte. */
static void answers_one_by_one(const char *const *requests, const int count, char *out, const size_t cap) {
    size_t total = 0;
    for (int i = 0; i < count; i++) {
        App app;
        int fds[2];
        Connection *conn;
        setup(&app, fds, &conn);
        send_all(fds[1], requests[i]);
        handle_readable(&app, conn);
        total += read_available(fds[1], out + total, cap - total);
        teardown(&app, fds);
    }
    out[total] = '\0';
    strip_dates(out);
}

static size_t join_requests(const char *const *requests, const int count, char *out, const size_t cap) {
    size_t off = 0;
    for (int i = 0; i < count; i++) {
        off += (size_t)snprintf(out + off, cap - off, "%s", requests[i]);
    }
    assert(off < cap);
    return off;
}

#define GET_A "GET /a HTTP/1.1\r\nHost: x\r\n\r\n"
#define GET_B "GET /b HTTP/1.1\r\nHost: x\r\n\r\n"
#define GET_C "GET /c HTTP/1.1\r\nHost: x\r\n\r\n"
#define GET_PING "GET /ping HTTP/1.1\r\nHost: x\r\n\r\n"
#define GET_KB "GET /kb HTTP/1.1\r\nHost: x\r\n\r\n"
#define GET_SHARED "GET /shared HTTP/1.1\r\nHost: x\r\n\r\n"

/* 16 pipelined GETs in one write (TechEmpower plaintext's shape): one write for all 16 answers,
 * byte-identical and in the same order as answering them one at a time. */
static void test_pipelined_batch_is_one_write(void) {
    const char *requests[MAX_PIPELINED_PER_EVENT];
    const char *const cycle[] = { GET_A, GET_B, GET_PING, GET_C };
    for (int i = 0; i < MAX_PIPELINED_PER_EVENT; i++) {
        requests[i] = cycle[i % 4];
    }
    static char expected[65536];
    answers_one_by_one(requests, MAX_PIPELINED_PER_EVENT, expected, sizeof(expected));

    App app;
    int fds[2];
    Connection *conn;
    setup_type(&app, fds, &conn, SOCK_DGRAM);
    char wire[4096];
    join_requests(requests, MAX_PIPELINED_PER_EVENT, wire, sizeof(wire));
    send_all(fds[1], wire);
    handle_readable(&app, conn);

    static char out[65536];
    assert(read_datagrams(fds[1], out, sizeof(out), NULL) == 1);
    strip_dates(out);
    assert(strcmp(out, expected) == 0);
    assert(app.connections[fds[0]] == conn);
    assert(app.batch_len == 0);
    assert(conn->in_buf == NULL && conn->in_len == 0 && conn->out_buf == NULL);
    assert(conn->events_watched & EVENT_READ && !(conn->events_watched & EVENT_WRITE));

    teardown(&app, fds);
}

/* A response that can't be coalesced (a shared_body sent by reference) in the middle: the batch goes out
 * in front of it in the same writev, then the last answer on its own - 2 writes, same bytes, same order. */
static void test_shared_body_in_the_middle_keeps_order(void) {
    const char *const requests[] = { GET_A, GET_B, GET_SHARED, GET_C };
    const int count = 4;
    static char expected[16384];
    answers_one_by_one(requests, count, expected, sizeof(expected));
    assert(strstr(expected, "\r\n\r\nhello") != NULL);

    App app;
    int fds[2];
    Connection *conn;
    setup_type(&app, fds, &conn, SOCK_DGRAM);
    char wire[1024];
    join_requests(requests, count, wire, sizeof(wire));
    send_all(fds[1], wire);
    handle_readable(&app, conn);

    static char out[16384];
    assert(read_datagrams(fds[1], out, sizeof(out), NULL) == 2);
    strip_dates(out);
    assert(strcmp(out, expected) == 0);
    assert(conn->shared_body == NULL && conn->out_buf == NULL && app.batch_len == 0);

    teardown(&app, fds);
}

/* Connection: close in the middle: the answers before it go out in the same write as its own, the
 * connection closes, and the request behind it is never answered. */
static void test_connection_close_in_the_middle_flushes_batch_then_closes(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_type(&app, fds, &conn, SOCK_DGRAM);
    send_all(fds[1], GET_A GET_PING "GET /b HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n" GET_C);
    handle_readable(&app, conn);

    char out[8192];
    assert(read_datagrams(fds[1], out, sizeof(out), NULL) == 1);
    assert(count_occurrences(out, "HTTP/1.1 200 OK") == 3);
    const char *a = strstr(out, "\r\n\r\n/a");
    const char *pong = strstr(out, "pong");
    const char *b = strstr(out, "\r\n\r\n/b");
    assert(a != NULL && pong != NULL && b != NULL && a < pong && pong < b);
    assert(strstr(out, "/c") == NULL);
    assert(count_occurrences(out, "Connection: close") == 1);
    assert(app.connections[fds[0]] == NULL);
    assert(app.batch_len == 0);

    teardown(&app, fds);
}

/* More answer bytes than BATCH_BUF_SIZE: whenever the next answer doesn't fit, it goes out with the batch
 * in front of it (joined in the arena) and a new batch starts. Fewer writes than answers, same bytes. */
static void test_batch_larger_than_the_buffer(void) {
    const char *requests[MAX_PIPELINED_PER_EVENT];
    for (int i = 0; i < MAX_PIPELINED_PER_EVENT; i++) {
        requests[i] = i % 2 == 0 ? GET_KB : GET_A;
    }
    static char expected[131072];
    answers_one_by_one(requests, MAX_PIPELINED_PER_EVENT, expected, sizeof(expected));
    assert(strlen(expected) > BATCH_BUF_SIZE);

    App app;
    int fds[2];
    Connection *conn;
    setup_type(&app, fds, &conn, SOCK_DGRAM);
    char wire[4096];
    join_requests(requests, MAX_PIPELINED_PER_EVENT, wire, sizeof(wire));
    send_all(fds[1], wire);
    handle_readable(&app, conn);

    static char out[131072];
    const int writes = read_datagrams(fds[1], out, sizeof(out), NULL);
    assert(writes >= 2 && writes < MAX_PIPELINED_PER_EVENT);
    strip_dates(out);
    assert(strcmp(out, expected) == 0);
    assert(app.connections[fds[0]] == conn && app.batch_len == 0);

    teardown(&app, fds);
}

/* Complete requests followed by half of the next: the batch goes out on its own (one write) and the half
 * stays buffered, not consumed; completing it gets it answered. */
static void test_batch_before_a_partial_request(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup_type(&app, fds, &conn, SOCK_DGRAM);
    const char *half = "GET /c HTTP/1.1\r\nHo";
    send_all(fds[1], GET_A GET_B "GET /c HTTP/1.1\r\nHo");
    handle_readable(&app, conn);

    char out[8192];
    assert(read_datagrams(fds[1], out, sizeof(out), NULL) == 1);
    const char *a = strstr(out, "\r\n\r\n/a");
    const char *b = strstr(out, "\r\n\r\n/b");
    assert(a != NULL && b != NULL && a < b && count_occurrences(out, "HTTP/1.1 200 OK") == 2);
    assert(app.connections[fds[0]] == conn && app.batch_len == 0);
    assert(conn->in_off == 0 && conn->in_len == strlen(half) && memcmp(conn->in_buf, half, strlen(half)) == 0);
    assert(conn->request_started != 0);

    send_all(fds[1], "st: x\r\n\r\n");
    handle_readable(&app, conn);
    assert(read_datagrams(fds[1], out, sizeof(out), NULL) == 1);
    assert(strstr(out, "\r\n\r\n/c") != NULL && count_occurrences(out, "HTTP/1.1 200 OK") == 1);
    assert(conn->in_len == 0);

    teardown(&app, fds);
}

/* A coalesced answer is on the wire before the 100 Continue for the request behind it. */
static void test_batch_goes_out_before_100_continue(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup(&app, fds, &conn);
    send_all(fds[1], GET_A "POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\n");
    handle_readable(&app, conn);

    char out[4096];
    read_available(fds[1], out, sizeof(out));
    const char *a = strstr(out, "\r\n\r\n/a");
    const char *cont = strstr(out, "HTTP/1.1 100 Continue\r\n\r\n");
    assert(a != NULL && cont != NULL && a < cont);

    send_all(fds[1], "hello");
    handle_readable(&app, conn);
    read_available(fds[1], out, sizeof(out));
    assert(strstr(out, "received 5 bytes") != NULL);
    assert(app.connections[fds[0]] == conn && conn->in_len == 0);

    teardown(&app, fds);
}

/* A batch flushed on its own that the socket can't take whole: the rest becomes the connection's pending
 * (owned) response, the partial request behind it stays buffered, and once it drains nothing was consumed
 * twice - the partial request is completed and answered after everything else, in order. */
static void test_batch_alone_hits_eagain_then_drains(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup(&app, fds, &conn);
    const int small = 4096;
    assert(setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0);
    assert(setsockopt(fds[1], SOL_SOCKET, SO_RCVBUF, &small, sizeof(small)) == 0);

    const char *half = "GET /c HTTP/1.1\r\nHo";
    send_all(fds[1], GET_KB GET_KB GET_KB GET_A GET_KB "GET /c HTTP/1.1\r\nHo");
    handle_readable(&app, conn);

    assert(app.connections[fds[0]] == conn && app.batch_len == 0);
    assert(conn->out_buf != NULL && conn->out_buf_owned && conn->out_sent < conn->out_len);
    assert(conn->request_len == 0); /* the batch's requests were consumed as they were queued */
    assert(conn->in_buf != app.read_buf && conn->in_len == strlen(half));
    assert(memcmp(conn->in_buf + conn->in_off, half, strlen(half)) == 0);
    assert(!(conn->events_watched & EVENT_READ) && (conn->events_watched & EVENT_WRITE));

    static char out[65536];
    size_t got = 0;
    for (int spins = 0; spins < 10000 && conn->out_buf != NULL; spins++) {
        got += read_available(fds[1], out + got, sizeof(out) - got);
        handle_writable(&app, conn);
    }
    assert(conn->out_buf == NULL);
    assert(conn->in_len == strlen(half) && memcmp(conn->in_buf + conn->in_off, half, strlen(half)) == 0);
    assert(conn->events_watched & EVENT_READ);

    send_all(fds[1], "st: x\r\n\r\n");
    handle_readable(&app, conn);
    got += read_available(fds[1], out + got, sizeof(out) - got);
    assert(count_occurrences(out, "HTTP/1.1 200 OK") == 6);
    const char *a = strstr(out, "\r\n\r\n/a");
    const char *c = strstr(out, "\r\n\r\n/c");
    assert(a != NULL && c != NULL && a < c);
    assert(count_occurrences(out, "\r\n\r\nkkk") == 4);
    assert(conn->in_len == 0 && conn->out_buf == NULL);

    teardown(&app, fds);
}

int main(void) {
    test_two_gets_in_one_write_both_answered_in_order();
    test_content_length_body_then_get();
    test_chunked_body_then_get();
    test_partial_second_request_is_kept_and_completed();
    test_per_event_cap_yields_then_resumes();
    test_connection_close_drops_the_rest();
    test_malformed_second_request_gets_400_after_first_answer();
    test_pending_response_holds_pipeline_until_drained();
    test_pipelined_batch_is_one_write();
    test_shared_body_in_the_middle_keeps_order();
    test_connection_close_in_the_middle_flushes_batch_then_closes();
    test_batch_larger_than_the_buffer();
    test_batch_before_a_partial_request();
    test_batch_goes_out_before_100_continue();
    test_batch_alone_hits_eagain_then_drains();
    printf("all pipelining tests passed\n");
    return 0;
}
