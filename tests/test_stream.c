/*
 * producer streaming (res_stream / stream_write / app_wake_streams). Pure framing tests against a
 * hand-built StreamWriter, then the real engine path over socketpair(2): handle_readable dispatches,
 * flush_connection calls the producer turn by turn, handle_writable / close_idle_connections /
 * app_wake_streams drive it the way the event loop does.
 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include "app_types.h"
#include "event_loop.h"
#include "connection.h"
#include "router.h"
#include "response.h"
#include "http_parser.h"

/* ---- producers used by the tests ---- */

typedef struct {
    int next;          /* next chunk index to emit */
    int total;         /* chunks to emit before STREAM_END */
    size_t chunk_len;  /* bytes per chunk */
    int abort_at;      /* >= 0: return STREAM_ABORT once next reaches it */
} CountingCtx;

static int ctx_frees;         /* how many times free_counting_ctx ran */
static int producer_calls;
static size_t max_turn_len;   /* largest single-turn output any producer call left in the writer */

static void free_counting_ctx(void *ctx) {
    ctx_frees++;
    free(ctx);
}

/* Byte i of chunk c: position-dependent so a dropped, repeated or reordered chunk is detectable. */
static char pattern_byte(const int chunk, const size_t i) {
    return (char)('a' + (int)(((size_t)chunk * 31u + i) % 26u));
}

static int counting_producer(StreamWriter *out, void *ctx_ptr) {
    CountingCtx *ctx = ctx_ptr;
    producer_calls++;
    char chunk[STREAM_WRITE_MAX];
    assert(ctx->chunk_len <= sizeof(chunk));
    while (ctx->next < ctx->total) {
        if (ctx->abort_at >= 0 && ctx->next >= ctx->abort_at) {
            return STREAM_ABORT;
        }
        for (size_t i = 0; i < ctx->chunk_len; i++) {
            chunk[i] = pattern_byte(ctx->next, i);
        }
        if (stream_write(out, chunk, ctx->chunk_len) != 0) {
            break; /* this turn's buffer is full: the same chunk is retried on the next call */
        }
        ctx->next++;
    }
    if (out->len > max_turn_len) {
        max_turn_len = out->len;
    }
    return ctx->next < ctx->total ? STREAM_MORE : STREAM_END;
}

static int stream_total_chunks = 3;
static size_t stream_chunk_len = 1000;
static int stream_abort_at = -1;

static void stream_handler(const Request *req, Response *res) {
    (void)req;
    CountingCtx *ctx = calloc(1, sizeof(*ctx));
    assert(ctx != NULL);
    ctx->total = stream_total_chunks;
    ctx->chunk_len = stream_chunk_len;
    ctx->abort_at = stream_abort_at;
    res_set_header(res, "Content-Type", "application/octet-stream");
    assert(res_stream(res, counting_producer, ctx, free_counting_ctx) == 0);
}

/* Server-sent events fed by a "publisher": a message is available when published > delivered. */
typedef struct {
    int delivered;
} SseCtx;

static int published;
static int sse_closed_by_server;

static int sse_producer(StreamWriter *out, void *ctx_ptr) {
    SseCtx *ctx = ctx_ptr;
    producer_calls++;
    while (ctx->delivered < published) {
        char event[64];
        const int n = snprintf(event, sizeof(event), "data: message %d\n\n", ctx->delivered);
        if (stream_write(out, event, (size_t)n) != 0) {
            return STREAM_MORE;
        }
        ctx->delivered++;
    }
    return sse_closed_by_server ? STREAM_END : STREAM_PAUSE;
}

static void sse_handler(const Request *req, Response *res) {
    (void)req;
    SseCtx *ctx = calloc(1, sizeof(*ctx));
    assert(ctx != NULL);
    res_set_header(res, "Content-Type", "text/event-stream");
    res_set_header(res, "Cache-Control", "no-cache");
    assert(res_stream(res, sse_producer, ctx, free_counting_ctx) == 0);
}

static void ping_handler(const Request *req, Response *res) {
    (void)req;
    res_send(res, "pong");
}

/* ---- harness ---- */

static void setup(App *app, int fds[2], Connection **conn) {
    app_init(app);
    assert(event_loop_init(app) == 0);
    app_get(app, "/stream", stream_handler);
    app_get(app, "/events", sse_handler);
    app_get(app, "/ping", ping_handler);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    assert(set_nonblocking(fds[0]) == 0);
    assert(set_nonblocking(fds[1]) == 0);
    *conn = connection_create(app, fds[0]);
    assert(*conn != NULL);
    app->connections[fds[0]] = *conn;
    app->open_connections++;
    assert(event_loop_watch_read(app, fds[0], *conn) == 0);
    ctx_frees = 0;
    producer_calls = 0;
    max_turn_len = 0;
    published = 0;
    sse_closed_by_server = 0;
    stream_total_chunks = 3;
    stream_chunk_len = 1000;
    stream_abort_at = -1;
}

static void teardown(App *app, int fds[2]) {
    if (fds[1] >= 0) {
        close(fds[1]);
    }
    app_destroy(app);
}

/* Growable receive buffer for everything the client side reads. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Received;

static void drain_client(const int fd, Received *rx) {
    while (1) {
        if (rx->cap - rx->len < 65536) {
            rx->cap = rx->cap == 0 ? 131072 : rx->cap * 2;
            rx->data = realloc(rx->data, rx->cap);
            assert(rx->data != NULL);
        }
        const ssize_t n = read(fd, rx->data + rx->len, rx->cap - rx->len - 1);
        if (n <= 0) {
            break;
        }
        rx->len += (size_t)n;
    }
    if (rx->data != NULL) {
        rx->data[rx->len] = '\0';
    }
}

/*
 * Decodes one chunked response starting at data (head included). Returns the offset just past the
 * terminating "0\r\n\r\n" (0 if the response is not complete yet); the decoded body goes to *body.
 */
static size_t decode_chunked_response(const char *data, const size_t len, char **body, size_t *body_len) {
    const char *head_end = strstr(data, "\r\n\r\n");
    if (head_end == NULL) {
        return 0;
    }
    size_t pos = (size_t)(head_end - data) + 4;
    *body = malloc(len + 1);
    assert(*body != NULL);
    *body_len = 0;
    while (pos < len) {
        char *end;
        const unsigned long size = strtoul(data + pos, &end, 16);
        if (end == data + pos || (size_t)(end - data) + 2 > len || end[0] != '\r' || end[1] != '\n') {
            break;
        }
        pos = (size_t)(end - data) + 2;
        if (size == 0) {
            if (pos + 2 <= len && data[pos] == '\r' && data[pos + 1] == '\n') {
                return pos + 2;
            }
            break;
        }
        if (pos + size + 2 > len) {
            break;
        }
        memcpy(*body + *body_len, data + pos, size);
        *body_len += size;
        assert(data[pos + size] == '\r' && data[pos + size + 1] == '\n');
        pos += size + 2;
    }
    free(*body);
    *body = NULL;
    return 0;
}

static void send_request(const int fd, const char *raw) {
    assert(write(fd, raw, strlen(raw)) == (ssize_t)strlen(raw));
}

/* ---- pure framing ---- */

static void test_stream_write_framing_and_bounds(void) {
    char buf[64];
    StreamWriter w = { buf, 0, sizeof(buf) };
    assert(stream_write(&w, "hello", 5) == 0);
    assert(w.len == 10 && memcmp(buf, "5\r\nhello\r\n", 10) == 0);
    assert(stream_write(&w, "x", 0) == 0 && w.len == 10); /* len 0: no-op, never a last-chunk */
    assert(stream_write(&w, NULL, 3) == -1 && w.len == 10);

    char big[40];
    memset(big, 'b', sizeof(big));
    assert(stream_write(&w, big, 40) == 0); /* "28\r\n" + 40 + "\r\n" = 46: 56 of 64 */
    assert(w.len == 56 && memcmp(buf + 10, "28\r\n", 4) == 0);
    assert(stream_write(&w, "abc", 3) == 0); /* "3\r\nabc\r\n" = 8: exactly the 8 left */
    assert(w.len == sizeof(buf));
}

static void test_stream_write_full_buffer_writes_nothing(void) {
    char buf[16];
    StreamWriter w = { buf, 0, sizeof(buf) };
    assert(stream_write(&w, "abcdefghi", 9) == 0); /* "9\r\n" + 9 + "\r\n" = 14 */
    assert(stream_write(&w, "x", 1) == -1);        /* needs 6, 2 left: refused whole */
    assert(w.len == 14 && memcmp(buf, "9\r\nabcdefghi\r\n", 14) == 0);
}

static void test_stream_write_max_fits_an_empty_turn(void) {
    char *buf = malloc(STREAM_CHUNK_SIZE);
    char *data = malloc(STREAM_WRITE_MAX + 1);
    assert(buf != NULL && data != NULL);
    memset(data, 'z', STREAM_WRITE_MAX + 1);
    /* the engine's writer: STREAM_CHUNK_SIZE minus the 5-byte last-chunk reserve */
    StreamWriter w = { buf, 0, STREAM_CHUNK_SIZE - 5 };
    assert(stream_write(&w, data, STREAM_WRITE_MAX) == 0);
    StreamWriter w2 = { buf, 0, STREAM_CHUNK_SIZE - 5 };
    assert(stream_write(&w2, data, STREAM_CHUNK_SIZE) == -1 && w2.len == 0);
    free(buf);
    free(data);
}

/* ---- res_stream on a Response ---- */

static void test_res_stream_commits_chunked_head_and_takes_ctx(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup(&app, fds, &conn);
    conn->keep_alive = 1;

    Response res;
    res_init(&res, conn);
    res_set_trailer(&res, "X-Checksum", "abc"); /* not supported on a producer stream: dropped */
    CountingCtx *ctx = calloc(1, sizeof(*ctx));
    assert(res_stream(&res, counting_producer, ctx, free_counting_ctx) == 0);
    assert(strstr(conn->out_buf, "Transfer-Encoding: chunked\r\n") != NULL);
    assert(strstr(conn->out_buf, "Trailer:") == NULL);
    assert(conn->stream_fn == counting_producer && conn->stream_ctx == ctx);
    assert(producer_calls == 0); /* never called inside the handler */

    const size_t head_len = conn->out_len;
    res_write(&res, "ignored", 7); /* the producer owns the body now */
    res_end(&res);
    assert(conn->out_len == head_len);
    assert(res_stream(&res, counting_producer, NULL, NULL) == -1); /* headers already sent */

    /* last wins: a res_send after res_stream drops the stream and frees its ctx exactly once */
    res_send(&res, "replaced");
    assert(conn->stream_fn == NULL && ctx_frees == 1);
    assert(strstr(conn->out_buf, "Content-Length: 8") != NULL);
    conn->out_buf = NULL; /* arena-resident: nothing to free */
    teardown(&app, fds);
    assert(ctx_frees == 1);
}

static void test_res_stream_head_request_frees_ctx_at_once(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup(&app, fds, &conn);

    Response res;
    res_init(&res, conn);
    res.is_head_request = 1;
    CountingCtx *ctx = calloc(1, sizeof(*ctx));
    assert(res_stream(&res, counting_producer, ctx, free_counting_ctx) == 0);
    assert(ctx_frees == 1 && conn->stream_fn == NULL);
    assert(strstr(conn->out_buf, "Transfer-Encoding: chunked") != NULL);
    conn->out_buf = NULL;
    teardown(&app, fds);
}

/* ---- through the engine ---- */

/* A 12 MiB body - past MAX_BODY_SIZE, which res_write cannot exceed - streamed with the connection's
 * output never holding more than one STREAM_CHUNK_SIZE turn, then a pipelined /ping answered after it. */
static void test_large_stream_is_bounded_and_keeps_the_connection(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup(&app, fds, &conn);
    stream_chunk_len = 4096;
    stream_total_chunks = (12 * 1024 * 1024) / 4096;

    send_request(fds[1], "GET /stream HTTP/1.1\r\nHost: x\r\n\r\nGET /ping HTTP/1.1\r\nHost: x\r\n\r\n");
    handle_readable(&app, conn);
    assert(app.connections[fds[0]] == conn);
    assert(conn->stream_fn != NULL && conn->stream_buf != NULL);

    Received rx = {0};
    int rounds = 0;
    while (conn->stream_fn != NULL || conn->out_buf != NULL) {
        assert(rounds++ < 200000);
        /* after the head: output is only ever the one turn buffer, never an accumulated body */
        if (conn->out_buf == conn->stream_buf) {
            assert(conn->out_len <= STREAM_CHUNK_SIZE);
        } else {
            assert(conn->out_len < 1024); /* the response head (arena, or its owned tail-copy) */
        }
        drain_client(fds[1], &rx);
        handle_writable(&app, conn);
        assert(app.connections[fds[0]] == conn);
    }
    drain_client(fds[1], &rx);

    char *body = NULL;
    size_t body_len = 0;
    const size_t end = decode_chunked_response(rx.data, rx.len, &body, &body_len);
    assert(end > 0);
    assert(body_len == (size_t)stream_total_chunks * stream_chunk_len);
    for (int c = 0; c < stream_total_chunks; c++) {
        for (size_t i = 0; i < stream_chunk_len; i += 511) {
            assert(body[(size_t)c * stream_chunk_len + i] == pattern_byte(c, i));
        }
    }
    assert(max_turn_len <= STREAM_CHUNK_SIZE - 5);
    assert(ctx_frees == 1);
    assert(conn->stream_buf == NULL); /* released when the stream ended */
    assert(strstr(rx.data, "Content-Type: application/octet-stream") != NULL);
    /* the pipelined request is answered after the last chunk, on the same connection */
    assert(strstr(rx.data + end, "HTTP/1.1 200 OK") == rx.data + end);
    assert(strstr(rx.data + end, "pong") != NULL);
    assert(conn->request_started == 0 && conn->last_write_progress == 0);

    free(body);
    free(rx.data);
    teardown(&app, fds);
    assert(ctx_frees == 1);
}

/* Server-sent events: the producer parks while nothing is published, app_wake_streams resumes it. */
static void test_paused_stream_waits_for_wake(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup(&app, fds, &conn);

    send_request(fds[1], "GET /events HTTP/1.1\r\nHost: x\r\n\r\n");
    handle_readable(&app, conn);
    Received rx = {0};
    drain_client(fds[1], &rx);
    assert(strstr(rx.data, "Content-Type: text/event-stream") != NULL);
    assert(conn->stream_fn != NULL && conn->stream_paused);
    assert(!(conn->events_watched & EVENT_WRITE)); /* parked: no busy write-readiness loop */
    assert(conn->events_watched & EVENT_READ);     /* but still watching for a hang-up */
    const int calls_after_open = producer_calls;

    /* nothing woke it: another writable turn would find it parked and not call the producer */
    handle_writable(&app, conn);
    assert(producer_calls == calls_after_open);

    published = 2;
    app_wake_streams(&app);
    assert(!conn->stream_paused && (conn->events_watched & EVENT_WRITE));
    assert(producer_calls == calls_after_open); /* only armed: called from the loop, not from the wake */
    handle_writable(&app, conn);
    drain_client(fds[1], &rx);
    assert(strstr(rx.data, "data: message 0\n\n") != NULL);
    assert(strstr(rx.data, "data: message 1\n\n") != NULL);
    assert(conn->stream_paused);

    sse_closed_by_server = 1;
    app_wake_streams(&app);
    handle_writable(&app, conn);
    drain_client(fds[1], &rx);
    char *body = NULL;
    size_t body_len = 0;
    assert(decode_chunked_response(rx.data, rx.len, &body, &body_len) == rx.len);
    assert(body_len == strlen("data: message 0\n\ndata: message 1\n\n"));
    assert(conn->stream_fn == NULL && ctx_frees == 1);
    assert(app.connections[fds[0]] == conn); /* keep-alive */
    assert(!(conn->events_watched & EVENT_WRITE));

    free(body);
    free(rx.data);
    teardown(&app, fds);
}

/* The once-a-second sweep resumes a parked stream instead of applying the write-stall deadline. */
static void test_sweep_resumes_paused_stream(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup(&app, fds, &conn);

    send_request(fds[1], "GET /events HTTP/1.1\r\nHost: x\r\n\r\n");
    handle_readable(&app, conn);
    assert(conn->stream_paused);
    conn->last_write_progress = time(NULL) - WRITE_TIMEOUT_SECONDS - 5; /* paused a long time */
    conn->last_activity = time(NULL) - IDLE_TIMEOUT_SECONDS - 5;

    close_idle_connections(&app);
    assert(app.connections[fds[0]] == conn);
    assert(!conn->stream_paused && (conn->events_watched & EVENT_WRITE));
    assert(time(NULL) - conn->last_write_progress < 2); /* stall clock restarted */

    published = 1;
    handle_writable(&app, conn);
    Received rx = {0};
    drain_client(fds[1], &rx);
    assert(strstr(rx.data, "data: message 0\n\n") != NULL);

    free(rx.data);
    teardown(&app, fds);
    assert(ctx_frees == 1); /* app_destroy closed the open stream and freed its ctx */
}

/* A client that hangs up while its stream is parked is closed at once, not at the next write. */
static void test_peer_hangup_while_paused_closes_and_frees_ctx(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup(&app, fds, &conn);

    send_request(fds[1], "GET /events HTTP/1.1\r\nHost: x\r\n\r\n");
    handle_readable(&app, conn);
    assert(conn->stream_paused && ctx_frees == 0);
    close(fds[1]);
    fds[1] = -1;

    handle_readable(&app, conn);
    assert(app.connections[fds[0]] == NULL);
    assert(ctx_frees == 1);
    teardown(&app, fds);
}

/* Bytes (a pipelined request) arriving while parked are left unread and read interest is dropped, so a
 * level-triggered loop does not spin on them; they are served after the stream ends. */
static void test_pipelined_bytes_while_paused_wait_for_the_stream(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup(&app, fds, &conn);

    send_request(fds[1], "GET /events HTTP/1.1\r\nHost: x\r\n\r\n");
    handle_readable(&app, conn);
    assert(conn->stream_paused);
    send_request(fds[1], "GET /ping HTTP/1.1\r\nHost: x\r\n\r\n");
    handle_readable(&app, conn);
    assert(app.connections[fds[0]] == conn);
    assert(!(conn->events_watched & EVENT_READ));
    assert(conn->in_buf == NULL); /* nothing consumed */

    sse_closed_by_server = 1;
    app_wake_streams(&app);
    handle_writable(&app, conn); /* stream ends; read interest is restored */
    assert(conn->stream_fn == NULL && (conn->events_watched & EVENT_READ));
    handle_readable(&app, conn);
    Received rx = {0};
    drain_client(fds[1], &rx);
    char *body = NULL;
    size_t body_len = 0;
    const size_t end = decode_chunked_response(rx.data, rx.len, &body, &body_len);
    assert(end > 0 && body_len == 0);
    assert(strstr(rx.data + end, "pong") != NULL);

    free(body);
    free(rx.data);
    teardown(&app, fds);
}

/* STREAM_ABORT closes without the last chunk: the client sees a truncated body, never a clean end. */
static void test_abort_truncates_and_closes(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup(&app, fds, &conn);
    stream_total_chunks = 100;
    stream_abort_at = 2;

    send_request(fds[1], "GET /stream HTTP/1.1\r\nHost: x\r\n\r\n");
    handle_readable(&app, conn);
    assert(app.connections[fds[0]] == NULL);
    assert(ctx_frees == 1);
    Received rx = {0};
    drain_client(fds[1], &rx);
    char *body = NULL;
    size_t body_len = 0;
    assert(decode_chunked_response(rx.data, rx.len, &body, &body_len) == 0); /* never completes */
    assert(strstr(rx.data, "\r\n0\r\n\r\n") == NULL);

    free(rx.data);
    teardown(&app, fds);
}

/* Graceful shutdown lets an in-flight stream finish (Connection: close afterwards) instead of cutting it. */
static void test_app_stop_keeps_stream_until_it_ends(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup(&app, fds, &conn);

    send_request(fds[1], "GET /events HTTP/1.1\r\nHost: x\r\n\r\n");
    handle_readable(&app, conn);
    assert(conn->stream_paused);
    app_stop(&app);
    assert(app.connections[fds[0]] == conn && conn->keep_alive == 0);

    sse_closed_by_server = 1;
    app_wake_streams(&app);
    handle_writable(&app, conn);
    assert(app.connections[fds[0]] == NULL); /* ended, then closed rather than kept alive */
    assert(ctx_frees == 1);
    Received rx = {0};
    drain_client(fds[1], &rx);
    char *body = NULL;
    size_t body_len = 0;
    assert(decode_chunked_response(rx.data, rx.len, &body, &body_len) == rx.len);

    free(body);
    free(rx.data);
    teardown(&app, fds);
}

/* HEAD through the engine: headers only, the producer never runs, the connection stays usable. */
static void test_head_request_through_engine(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup(&app, fds, &conn);

    send_request(fds[1], "HEAD /stream HTTP/1.1\r\nHost: x\r\n\r\n");
    handle_readable(&app, conn);
    Received rx = {0};
    drain_client(fds[1], &rx);
    assert(strstr(rx.data, "Transfer-Encoding: chunked") != NULL);
    assert(strstr(rx.data, "\r\n\r\n")[4] == '\0');
    assert(producer_calls == 0 && ctx_frees == 1);
    assert(app.connections[fds[0]] == conn && conn->stream_fn == NULL);

    free(rx.data);
    teardown(&app, fds);
}

/* a producer stream on a bodiless status (here 204) never runs: head only, ctx freed at once. */
static void test_res_stream_bodiless_status_frees_ctx_at_once(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup(&app, fds, &conn);

    Response res;
    res_init(&res, conn);
    res_status(&res, 204);
    CountingCtx *ctx = calloc(1, sizeof(*ctx));
    assert(res_stream(&res, counting_producer, ctx, free_counting_ctx) == 0);
    assert(ctx_frees == 1 && conn->stream_fn == NULL);
    assert(strstr(conn->out_buf, "Transfer-Encoding") == NULL);
    assert(strstr(conn->out_buf, "\r\n\r\n")[4] == '\0'); /* nothing after the head */
    conn->out_buf = NULL;
    teardown(&app, fds);
}

int main(void) {
    test_res_stream_bodiless_status_frees_ctx_at_once();
    test_stream_write_framing_and_bounds();
    test_stream_write_full_buffer_writes_nothing();
    test_stream_write_max_fits_an_empty_turn();
    test_res_stream_commits_chunked_head_and_takes_ctx();
    test_res_stream_head_request_frees_ctx_at_once();
    test_large_stream_is_bounded_and_keeps_the_connection();
    test_paused_stream_waits_for_wake();
    test_sweep_resumes_paused_stream();
    test_peer_hangup_while_paused_closes_and_frees_ctx();
    test_pipelined_bytes_while_paused_wait_for_the_stream();
    test_abort_truncates_and_closes();
    test_app_stop_keeps_stream_until_it_ends();
    test_head_request_through_engine();
    printf("all stream tests passed\n");
    return 0;
}
