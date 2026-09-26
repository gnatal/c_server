/*
 * Deferred responses (res_defer / res_resume / req_deferred) and application fds (app_watch_fd), through the real
 * event loop: clients are socketpair(2) ends registered as Connections, the "database" is another socketpair whose
 * server end the app watches, and app_run_once(app, 0) drives everything the way app_listen_worker does. Writing
 * N bytes to the database's far end answers the N oldest pending queries.
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
#include "arena.h"
#include "event_loop.h"
#include "connection.h"
#include "router.h"
#include "response.h"
#include "http_parser.h"

/* ---- the fake database: a FIFO of pending queries answered by bytes on a socketpair ---- */

#define MAX_PENDING 256

typedef struct {
    DeferHandle h;
    const char *note; /* allocated from the arena before res_defer: must survive until the resume */
} Pending;

static Pending pending[MAX_PENDING];
static int pending_head;
static int pending_tail;
static int resumed_stale;  /* res_resume returned NULL: the request was gone */
static int defer_refused;  /* res_defer returned 0 */
static int db_callbacks;
static int stray_answers;  /* bytes with no pending query (tests of the watch itself) */
static int db[2];          /* db[0]: watched by the app; db[1]: the test answers through it */

static void on_db_ready(App *app, int fd, unsigned events, void *udata) {
    assert(udata == &pending);
    db_callbacks++;
    if (!(events & WATCH_READ)) {
        return;
    }
    char answers[MAX_PENDING];
    const ssize_t n = read(fd, answers, sizeof(answers));
    for (ssize_t i = 0; i < n; i++) {
        if (pending_head == pending_tail) {
            stray_answers++;
            continue;
        }
        const Pending p = pending[pending_head++];
        Response *res = res_resume(app, p.h);
        if (res == NULL) {
            resumed_stale++;
            assert(req_deferred(app, p.h) == NULL);
            continue;
        }
        const Request *req = req_deferred(app, p.h);
        assert(req != NULL);
        const char *id = req_get_query(req, "id");
        char body[128];
        snprintf(body, sizeof(body), "id=%s %s body=%.*s ua=%s", id != NULL ? id : "-", p.note,
                 req->content_length, req->body, req_get_header(req, "User-Agent") != NULL ? "yes" : "no");
        res_send(res, body);
    }
}

/* ---- handlers ---- */

static void db_handler(const Request *req, Response *res) {
    res_set_header(res, "X-Before", "kept"); /* set before deferring: must be in the final answer */
    char *note = arena_alloc(res->conn->arena, 32);
    assert(note != NULL);
    const char *id = req_get_query(req, "id");
    snprintf(note, 32, "note-%s", id != NULL ? id : "-");
    const DeferHandle h = res_defer(res);
    if (h == 0) {
        defer_refused++;
        return;
    }
    assert(res_defer(res) == 0); /* already deferred */
    assert(req_deferred(res->conn->app, h) == NULL); /* while the handler runs, use its own req */
    assert(pending_tail < MAX_PENDING);
    pending[pending_tail++] = (Pending){h, note};
}

static void ping_handler(const Request *req, Response *res) {
    (void)req;
    res_send(res, "pong");
}

static void medium_handler(const Request *req, Response *res) {
    (void)req;
    char body[1001];
    memset(body, 'm', 1000);
    body[1000] = '\0';
    res_send(res, body);
}

static DeferHandle sync_handle;

/* defers, then answers at once through res_resume */
static void sync_handler(const Request *req, Response *res) {
    (void)req;
    sync_handle = res_defer(res);
    assert(sync_handle != 0);
    Response *again = res_resume(res->conn->app, sync_handle);
    assert(again == res);
    res_send(again, "sync");
}

/* defers, then answers directly without resuming */
static void change_mind_handler(const Request *req, Response *res) {
    (void)req;
    sync_handle = res_defer(res);
    assert(sync_handle != 0);
    res_send(res, "changed");
}

/* answers first: res_defer must refuse and leave the response alone */
static void late_defer_handler(const Request *req, Response *res) {
    (void)req;
    res_send(res, "early");
    assert(res_defer(res) == 0);
}

/* resumes (from the test) without sending anything */
static void silent_handler(const Request *req, Response *res) {
    (void)req;
    sync_handle = res_defer(res);
    assert(sync_handle != 0);
}

/* ---- harness ---- */

typedef struct {
    int fds[2];      /* fds[0]: the server's Connection; fds[1]: the client */
    Connection *conn;
    char rx[65536];
    size_t rx_len;
} Client;

static void setup(App *app) {
    app_init(app);
    assert(event_loop_init(app) == 0);
    app_get(app, "/db", db_handler);
    app_post(app, "/db", db_handler);
    app_get(app, "/ping", ping_handler);
    app_get(app, "/medium", medium_handler);
    app_get(app, "/sync", sync_handler);
    app_get(app, "/change", change_mind_handler);
    app_get(app, "/late", late_defer_handler);
    app_get(app, "/silent", silent_handler);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, db) == 0);
    assert(set_nonblocking(db[0]) == 0);
    assert(set_nonblocking(db[1]) == 0);
    assert(app_watch_fd(app, db[0], WATCH_READ, on_db_ready, &pending) == 0);
    pending_head = 0;
    pending_tail = 0;
    resumed_stale = 0;
    defer_refused = 0;
    db_callbacks = 0;
    stray_answers = 0;
    sync_handle = 0;
}

static void open_client(App *app, Client *c) {
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, c->fds) == 0);
    assert(set_nonblocking(c->fds[0]) == 0);
    assert(set_nonblocking(c->fds[1]) == 0);
    c->conn = connection_create(app, c->fds[0]);
    assert(c->conn != NULL);
    app->connections[c->fds[0]] = c->conn;
    app->open_connections++;
    assert(event_loop_watch_read(app, c->fds[0], c->conn) == 0);
    c->rx_len = 0;
    c->rx[0] = '\0';
}

static int is_open(const App *app, const Client *c) {
    return app->connections[c->fds[0]] == c->conn;
}

static void teardown(App *app) {
    assert(app_unwatch_fd(app, db[0]) == 0);
    close(db[0]);
    close(db[1]);
    app_destroy(app);
}

/* runs the event loop until nothing happens for a few turns */
static void pump(App *app) {
    int idle = 0;
    for (int turns = 0; turns < 1000 && idle < 3; turns++) {
        const int n = app_run_once(app, 0);
        assert(n >= 0 || n == LOOP_TURN_EXIT);
        idle = n == 0 ? idle + 1 : 0;
    }
}

static void send_str(const Client *c, const char *raw) {
    assert(write(c->fds[1], raw, strlen(raw)) == (ssize_t)strlen(raw));
}

/* appends whatever the client can read; returns 1 if the server closed its end (EOF) */
static int receive(Client *c) {
    while (c->rx_len < sizeof(c->rx) - 1) {
        const ssize_t n = read(c->fds[1], c->rx + c->rx_len, sizeof(c->rx) - 1 - c->rx_len);
        if (n == 0) {
            c->rx[c->rx_len] = '\0';
            return 1;
        }
        if (n < 0) {
            break;
        }
        c->rx_len += (size_t)n;
    }
    c->rx[c->rx_len] = '\0';
    return 0;
}

static void answer_db(const int count) {
    char bytes[MAX_PENDING];
    memset(bytes, 'r', (size_t)count);
    assert(write(db[1], bytes, (size_t)count) == count);
}

static int count_of(const char *hay, const char *needle) {
    int n = 0;
    for (const char *p = strstr(hay, needle); p != NULL; p = strstr(p + 1, needle)) {
        n++;
    }
    return n;
}

/* offset of the first occurrence of needle at or after `from`, asserting it exists */
static size_t find_from(const char *hay, const size_t from, const char *needle) {
    const char *p = strstr(hay + from, needle);
    assert(p != NULL);
    return (size_t)(p - hay);
}

/* ---- tests ---- */

/* The fake-database round trip: the handler defers, nothing is answered until the "database" replies, then the
 * response arrives with the headers set before deferring, the kept request's query/headers/body, and memory the
 * handler allocated before deferring. The connection is back to plain keep-alive, holding nothing. */
static void test_deferred_response_waits_for_the_watched_fd(void) {
    App app;
    setup(&app);
    Client c;
    open_client(&app, &c);

    send_str(&c, "POST /db?id=7 HTTP/1.1\r\nHost: x\r\nUser-Agent: t\r\nContent-Length: 5\r\n\r\nhello");
    pump(&app);
    assert(receive(&c) == 0 && c.rx_len == 0); /* nothing yet */
    assert(c.conn->deferred != NULL && c.conn->deferred->state == DEFER_WAITING);
    assert(pending_tail == 1);
    assert(c.conn->in_buf == NULL);                  /* the request's bytes moved into the deferred arena */
    assert(!(c.conn->events_watched & EVENT_WRITE)); /* nothing pending on the socket */
    assert(c.conn->events_watched & EVENT_READ);     /* watching for a hang-up */
    assert(app.buffered_bytes >= ARENA_SIZE);        /* charged to the budget */
    assert(app.arena.buf != c.conn->deferred->arena.buf); /* the worker arena moved on to another block */

    answer_db(1);
    assert(app_run_once(&app, 0) == 1); /* one turn: the database event, and the answer goes out in that turn */
    assert(receive(&c) == 0);
    assert(strncmp(c.rx, "HTTP/1.1 200 OK\r\n", 17) == 0);
    assert(strstr(c.rx, "X-Before: kept\r\n") != NULL);
    assert(strstr(c.rx, "Connection: keep-alive\r\n") != NULL);
    assert(strstr(c.rx, "id=7 note-7 body=hello ua=yes") != NULL);
    assert(is_open(&app, &c) && c.conn->deferred == NULL && c.conn->arena == &app.arena);
    assert(app.buffered_bytes == 0);
    assert(app.arena_pool.spare == 1); /* the kept arena's block came back */
    assert(db_callbacks >= 1 && resumed_stale == 0);

    /* the connection keeps serving */
    c.rx_len = 0;
    send_str(&c, "GET /ping HTTP/1.1\r\nHost: x\r\n\r\n");
    pump(&app);
    receive(&c);
    assert(strstr(c.rx, "pong") != NULL);
    teardown(&app);
}

/* Two pipelined requests where the first defers: the second is not answered first, and is answered (in order)
 * right after the first. A third request arriving while deferred waits too. */
static void test_pipelined_request_waits_behind_deferred(void) {
    App app;
    setup(&app);
    Client c;
    open_client(&app, &c);

    send_str(&c, "GET /db?id=1 HTTP/1.1\r\nHost: x\r\n\r\nGET /ping HTTP/1.1\r\nHost: x\r\n\r\n");
    pump(&app);
    receive(&c);
    assert(c.rx_len == 0); /* /ping must not overtake the deferred answer */
    assert(c.conn->in_len > c.conn->in_off); /* /ping stays buffered (copied out of the shared read buffer) */
    assert(c.conn->in_buf != app.read_buf);

    send_str(&c, "GET /ping HTTP/1.1\r\nHost: x\r\n\r\n"); /* arrives while deferred: left in the socket */
    pump(&app);
    receive(&c);
    assert(c.rx_len == 0);
    assert(!(c.conn->events_watched & EVENT_READ)); /* peeked, read interest dropped: no spin */

    answer_db(1);
    pump(&app);
    receive(&c);
    assert(count_of(c.rx, "HTTP/1.1 200") == 3);
    const size_t first = find_from(c.rx, 0, "id=1");
    const size_t second = find_from(c.rx, first, "pong");
    find_from(c.rx, second + 4, "pong");
    assert(is_open(&app, &c) && c.conn->in_len == 0);
    teardown(&app);
}

/* Responses coalesced before a deferred request go out when it parks (they do not wait for the database), and the
 * order on the wire stays the request order. */
static void test_coalesced_batch_goes_out_before_the_deferred_request(void) {
    App app;
    setup(&app);
    Client c;
    open_client(&app, &c);

    send_str(&c, "GET /ping HTTP/1.1\r\nHost: x\r\n\r\nGET /ping HTTP/1.1\r\nHost: x\r\n\r\n"
                 "GET /db?id=2 HTTP/1.1\r\nHost: x\r\n\r\nGET /ping HTTP/1.1\r\nHost: x\r\n\r\n");
    pump(&app);
    receive(&c);
    assert(count_of(c.rx, "HTTP/1.1 200") == 2 && count_of(c.rx, "pong") == 2); /* the batch, not the rest */
    assert(c.conn->deferred != NULL && c.conn->deferred->prefix == NULL);

    answer_db(1);
    pump(&app);
    receive(&c);
    assert(count_of(c.rx, "HTTP/1.1 200") == 4);
    const size_t deferred_at = find_from(c.rx, 0, "id=2");
    assert(count_of(c.rx + deferred_at, "pong") == 1); /* exactly one pong after it */

    /* the deferred request asks to close: the batch in front of it is still written as keep-alive (the
     * connection must stay open for the deferred answer), which then carries Connection: close */
    c.rx_len = 0;
    send_str(&c, "GET /ping HTTP/1.1\r\nHost: x\r\n\r\nGET /db?id=9 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    pump(&app);
    assert(receive(&c) == 0 && strstr(c.rx, "pong") != NULL);
    assert(is_open(&app, &c) && c.conn->deferred != NULL);
    answer_db(1);
    pump(&app);
    assert(receive(&c) == 1);
    assert(strstr(c.rx, "id=9") != NULL);
    assert(count_of(c.rx, "Connection: keep-alive") == 1 && count_of(c.rx, "Connection: close") == 1);
    assert(find_from(c.rx, 0, "Connection: close") > find_from(c.rx, 0, "pong")); /* on the deferred answer */
    assert(!is_open(&app, &c));
    teardown(&app);
}

/* The batch in front of a deferred request does not fit the socket: its unsent tail is held (dr->prefix) instead of
 * staying pending, and goes out in front of the deferred response. Byte order is still request order. */
static void test_unsent_batch_tail_is_written_before_the_deferred_response(void) {
    App app;
    setup(&app);
    Client c;
    open_client(&app, &c);
    const int small = 2048;
    assert(setsockopt(c.fds[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0);
    assert(setsockopt(c.fds[1], SOL_SOCKET, SO_RCVBUF, &small, sizeof(small)) == 0);

    char raw[4096] = "";
    for (int i = 0; i < 12; i++) {
        strncat(raw, "GET /medium HTTP/1.1\r\nHost: x\r\n\r\n", sizeof(raw) - strlen(raw) - 1);
    }
    strncat(raw, "GET /db?id=3 HTTP/1.1\r\nHost: x\r\n\r\n", sizeof(raw) - strlen(raw) - 1);
    send_str(&c, raw);
    pump(&app);
    assert(c.conn->deferred != NULL);
    assert(c.conn->deferred->prefix != NULL && c.conn->deferred->prefix_len > 0); /* the socket took only part */
    assert(c.conn->out_buf == NULL && !(c.conn->events_watched & EVENT_WRITE));
    assert(app.buffered_bytes >= ARENA_SIZE + c.conn->deferred->prefix_len);

    receive(&c); /* the client reads what was sent; the rest waits for the deferred response */
    answer_db(1);
    for (int i = 0; i < 200 && count_of(c.rx, "id=3") == 0; i++) {
        pump(&app);
        receive(&c);
    }
    assert(count_of(c.rx, "HTTP/1.1 200") == 13);
    size_t pos = 0;
    for (int i = 0; i < 12; i++) {
        pos = find_from(c.rx, pos, "mmmmmmmmmm") + 1000;
    }
    find_from(c.rx, pos, "id=3");
    assert(is_open(&app, &c) && app.buffered_bytes == 0);
    teardown(&app);
}

/* The client hangs up while deferred: the connection closes at once, the deferred memory is released, and the
 * later database answer finds a stale handle (res_resume NULL) instead of a freed Connection. Run under ASan. */
static void test_client_close_while_deferred(void) {
    App app;
    setup(&app);
    Client c;
    open_client(&app, &c);

    send_str(&c, "GET /db?id=4 HTTP/1.1\r\nHost: x\r\n\r\n");
    pump(&app);
    assert(c.conn->deferred != NULL);
    close(c.fds[1]);
    pump(&app);
    assert(!is_open(&app, &c));
    assert(app.buffered_bytes == 0 && app.arena_pool.spare == 1);

    answer_db(1);
    pump(&app);
    assert(resumed_stale == 1);
    teardown(&app);
}

/* A stale handle whose slot now belongs to a newer deferred request must not reach it: the slot generation
 * moved on. Without that, the old handle's answer would go to the new client. */
static void test_stale_handle_never_reaches_a_reused_slot(void) {
    App app;
    setup(&app);
    Client first;
    Client second;
    open_client(&app, &first);
    open_client(&app, &second);

    send_str(&first, "GET /db?id=10 HTTP/1.1\r\nHost: x\r\n\r\n");
    pump(&app);
    close(first.fds[1]); /* gone: its slot is freed */
    pump(&app);
    send_str(&second, "GET /db?id=11 HTTP/1.1\r\nHost: x\r\n\r\n");
    pump(&app);
    assert((pending[0].h & 0xFFFFFFFFu) == (pending[1].h & 0xFFFFFFFFu)); /* same slot, reused */
    assert(pending[0].h != pending[1].h);

    answer_db(2); /* the stale handle first, then the live one */
    pump(&app);
    assert(resumed_stale == 1);
    receive(&second);
    assert(count_of(second.rx, "HTTP/1.1 200") == 1 && strstr(second.rx, "id=11 note-11") != NULL);
    teardown(&app);
}

/* Not resumed within DEFER_TIMEOUT_SECONDS: 504 with Connection: close, connection closed, handle stale. */
static void test_defer_deadline_answers_504(void) {
    App app;
    setup(&app);
    Client c;
    open_client(&app, &c);

    send_str(&c, "GET /db?id=5 HTTP/1.1\r\nHost: x\r\n\r\nGET /ping HTTP/1.1\r\nHost: x\r\n\r\n");
    pump(&app);
    assert(c.conn->deferred != NULL);
    c.conn->last_write_progress = time(NULL) - WRITE_TIMEOUT_SECONDS - 5; /* no stall rule applies */
    c.conn->last_activity = time(NULL) - IDLE_TIMEOUT_SECONDS - 5;       /* no idle rule applies */
    close_idle_connections(&app);
    assert(is_open(&app, &c)); /* only its own deadline counts */

    c.conn->deferred->deadline = time(NULL) - 1;
    close_idle_connections(&app);
    assert(!is_open(&app, &c));
    pump(&app); /* io_uring: the socket is really closed once its poll removal is submitted, on the next poll */
    assert(receive(&c) == 1);
    assert(strncmp(c.rx, "HTTP/1.1 504 ", 13) == 0);
    assert(strstr(c.rx, "Connection: close\r\n") != NULL);
    assert(strstr(c.rx, "X-Before") == NULL); /* what the handler prepared is dropped */
    assert(count_of(c.rx, "HTTP/1.1") == 1);  /* the pipelined /ping is dropped with the connection */
    assert(app.buffered_bytes == 0);

    answer_db(1);
    pump(&app);
    assert(resumed_stale == 1);
    teardown(&app);
}

/* app_stop while deferred: the connection is in flight, not idle, so it stays open; the response still arrives,
 * with Connection: close, and then the worker has drained. */
static void test_app_stop_lets_the_deferred_response_finish(void) {
    App app;
    setup(&app);
    Client c;
    open_client(&app, &c);

    send_str(&c, "GET /db?id=6 HTTP/1.1\r\nHost: x\r\n\r\n");
    pump(&app);
    app_stop(&app);
    assert(is_open(&app, &c) && c.conn->keep_alive == 0);

    answer_db(1);
    int exit_seen = 0;
    for (int i = 0; i < 20 && !exit_seen; i++) {
        exit_seen = app_run_once(&app, 0) == LOOP_TURN_EXIT;
    }
    assert(exit_seen); /* drained */
    app_run_once(&app, 0); /* io_uring: submits the closed socket's poll removal, which really closes it */
    assert(receive(&c) == 1);
    assert(strstr(c.rx, "id=6") != NULL && strstr(c.rx, "Connection: close\r\n") != NULL);
    assert(app_count_connections(&app) == 0);
    teardown(&app);
}

/* Over ServerConfig.max_buffered_bytes, res_defer refuses and the client gets 503 at once. */
static void test_over_budget_defer_gets_503(void) {
    App app;
    setup(&app);
    app.config.max_buffered_bytes = ARENA_SIZE; /* a deferred request needs ARENA_SIZE + its own bytes */
    Client c;
    open_client(&app, &c);

    send_str(&c, "GET /db?id=8 HTTP/1.1\r\nHost: x\r\n\r\n");
    pump(&app);
    receive(&c);
    assert(defer_refused == 1 && pending_tail == 0);
    assert(strncmp(c.rx, "HTTP/1.1 503 ", 13) == 0);
    assert(is_open(&app, &c) && c.conn->deferred == NULL && app.buffered_bytes == 0);
    teardown(&app);
}

/* Many deferred requests on many connections at once, answered in one burst: each gets its own answer, pool
 * blocks are reused, nothing is left charged. Run under ASan. */
static void test_many_concurrent_deferred_requests(void) {
    enum { N = 100, ROUNDS = 3 };
    App app;
    setup(&app);
    static Client clients[N];
    for (int i = 0; i < N; i++) {
        open_client(&app, &clients[i]);
    }
    for (int round = 0; round < ROUNDS; round++) {
        pending_head = 0;
        pending_tail = 0;
        for (int i = 0; i < N; i++) {
            char raw[96];
            snprintf(raw, sizeof(raw), "GET /db?id=%d HTTP/1.1\r\nHost: x\r\n\r\n", round * 1000 + i);
            clients[i].rx_len = 0;
            send_str(&clients[i], raw);
        }
        pump(&app);
        assert(pending_tail == N);
        assert(app.buffered_bytes >= (size_t)N * ARENA_SIZE);
        answer_db(N);
        pump(&app);
        for (int i = 0; i < N; i++) {
            receive(&clients[i]);
            char want[32];
            snprintf(want, sizeof(want), "id=%d note-%d ", round * 1000 + i, round * 1000 + i);
            assert(strstr(clients[i].rx, want) != NULL);
            assert(count_of(clients[i].rx, "HTTP/1.1") == 1);
            assert(is_open(&app, &clients[i]));
        }
        assert(app.buffered_bytes == 0);
        assert(app.arena_pool.spare <= ARENA_POOL_MAX_SPARE);
    }
    assert(resumed_stale == 0);
    teardown(&app);
}

/* A handler may defer and still answer before returning: through res_resume, or with a plain res_* send. Either
 * is written at once and the handle goes stale. A handler that already answered cannot defer. */
static void test_answered_inside_the_handler(void) {
    App app;
    setup(&app);
    Client c;
    open_client(&app, &c);

    send_str(&c, "GET /sync HTTP/1.1\r\nHost: x\r\n\r\n");
    pump(&app);
    receive(&c);
    assert(strstr(c.rx, "sync") != NULL && c.conn->deferred == NULL);
    assert(res_resume(&app, sync_handle) == NULL);

    c.rx_len = 0;
    send_str(&c, "GET /change HTTP/1.1\r\nHost: x\r\n\r\n");
    pump(&app);
    receive(&c);
    assert(strstr(c.rx, "changed") != NULL && c.conn->deferred == NULL);
    assert(res_resume(&app, sync_handle) == NULL);

    c.rx_len = 0;
    send_str(&c, "GET /late HTTP/1.1\r\nHost: x\r\n\r\n");
    pump(&app);
    receive(&c);
    assert(strstr(c.rx, "early") != NULL);
    assert(app.buffered_bytes == 0 && is_open(&app, &c));
    teardown(&app);
}

/* Resumed without a response: 500, not a silent hang. res_resume twice returns the same Response. Stale and
 * garbage handles resolve to nothing. */
static void test_resume_without_sending_answers_500(void) {
    App app;
    setup(&app);
    Client c;
    open_client(&app, &c);

    assert(res_resume(&app, 0) == NULL);
    assert(res_resume(&app, 0xFFFFFFFF00000001ull) == NULL); /* slot table not even allocated yet */
    send_str(&c, "GET /silent HTTP/1.1\r\nHost: x\r\n\r\n");
    pump(&app);
    Response *first = res_resume(&app, sync_handle);
    assert(first != NULL && res_resume(&app, sync_handle) == first);
    assert(res_resume(&app, sync_handle + (1ull << 32)) == NULL); /* right slot, wrong generation */
    pump(&app);
    receive(&c);
    assert(strncmp(c.rx, "HTTP/1.1 500 ", 13) == 0);
    assert(res_resume(&app, sync_handle) == NULL);
    teardown(&app);
}

/* app_watch_fd: interest changes, write readiness, unwatch stops callbacks, and refusals. */
static int write_ready_calls;
static void on_write_ready(App *app, int fd, unsigned events, void *udata) {
    (void)udata;
    if (events & WATCH_WRITE) {
        write_ready_calls++;
        assert(app_watch_fd(app, fd, WATCH_READ, on_db_ready, &pending) == 0); /* back to read only */
    }
}

static void test_watch_fd_interest_and_refusals(void) {
    App app;
    setup(&app);
    Client c;
    open_client(&app, &c);

    assert(app_watch_fd(&app, c.fds[0], WATCH_READ, on_db_ready, NULL) == -1); /* a client connection */
    assert(app_watch_fd(&app, -1, WATCH_READ, on_db_ready, NULL) == -1);
    assert(app_watch_fd(&app, db[0], WATCH_READ, NULL, NULL) == -1);
    assert(app_watch_fd(&app, db[0], WATCH_ERROR, on_db_ready, NULL) == -1);
    assert(app_unwatch_fd(&app, c.fds[0]) == -1);

    /* a connected socket is writable: WATCH_WRITE fires once, then the callback drops it */
    assert(app_watch_fd(&app, db[0], WATCH_READ | WATCH_WRITE, on_write_ready, NULL) == 0);
    assert(app.watched[db[0]].events_watched == (EVENT_READ | EVENT_WRITE));
    pump(&app);
    assert(write_ready_calls == 1);
    assert(app.watched[db[0]].events_watched == EVENT_READ);

    /* unwatched: bytes on it are not reported */
    assert(app_unwatch_fd(&app, db[0]) == 0);
    assert(app_unwatch_fd(&app, db[0]) == -1);
    answer_db(1);
    const int before = db_callbacks;
    pump(&app);
    assert(db_callbacks == before);

    /* watched again (fresh registration after a removal) */
    assert(app_watch_fd(&app, db[0], WATCH_READ, on_db_ready, &pending) == 0);
    pump(&app);
    assert(db_callbacks == before + 1 && stray_answers == 1);
    teardown(&app);
}

/* the peer of a watched socket hangs up: reported (WATCH_READ, and WATCH_ERROR where the backend says so) */
static unsigned hangup_events;
static void on_hangup(App *app, int fd, unsigned events, void *udata) {
    (void)udata;
    hangup_events |= events;
    char buf[8];
    if (read(fd, buf, sizeof(buf)) == 0) {
        assert(app_unwatch_fd(app, fd) == 0);
    }
}

static void test_watched_peer_hangup_is_reported(void) {
    App app;
    setup(&app);
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    assert(set_nonblocking(pair[0]) == 0);
    assert(app_watch_fd(&app, pair[0], WATCH_READ, on_hangup, NULL) == 0);
    close(pair[1]);
    hangup_events = 0;
    pump(&app);
    assert(hangup_events & WATCH_READ);
    assert(app.watched[pair[0]].fn == NULL); /* the callback unwatched it on EOF */
    close(pair[0]);
    teardown(&app);
}

/* ---- app_on_turn_end ---- */

static int turn_end_calls;
static int pending_at_turn_end; /* how many requests had deferred when the hook last ran */

static void count_turn_end(App *app, void *udata) {
    (void)app;
    assert(udata == &turn_end_calls);
    turn_end_calls++;
    pending_at_turn_end = pending_tail;
}

/* resumes and answers every pending query: a turn-end "flush" that finishes work itself */
static void answer_at_turn_end(App *app, void *udata) {
    (void)udata;
    turn_end_calls++;
    while (pending_head != pending_tail) {
        Response *res = res_resume(app, pending[pending_head++].h);
        assert(res != NULL);
        res_send(res, "from-turn-end");
    }
}

/* The hook runs exactly once per turn, after every handler of that turn (three pipelined requests deferred in one
 * read event), also on a turn with no events, with its udata; replaced or removed, the old one no longer runs. */
static void test_turn_end_hook_runs_once_per_turn_after_handlers(void) {
    App app;
    setup(&app);
    assert(app.turn_end_hook == NULL);
    Client c;
    open_client(&app, &c);
    turn_end_calls = 0;
    pending_at_turn_end = -1;
    app_on_turn_end(&app, count_turn_end, &turn_end_calls);

    send_str(&c, "GET /db?id=1 HTTP/1.1\r\nHost: x\r\n\r\nGET /ping HTTP/1.1\r\nHost: x\r\n\r\n");
    Client d, e;
    open_client(&app, &d);
    open_client(&app, &e);
    send_str(&d, "GET /db?id=2 HTTP/1.1\r\nHost: x\r\n\r\n");
    send_str(&e, "GET /db?id=3 HTTP/1.1\r\nHost: x\r\n\r\n");
    int turns = 0;
    int idle_turns = 0;
    while ((pending_tail < 3 || idle_turns == 0) && turns < 100) {
        const int n = app_run_once(&app, 0);
        assert(n >= 0);
        turns++;
        idle_turns += n == 0;
        assert(turn_end_calls == turns);             /* once per turn, idle turns included */
        assert(pending_at_turn_end == pending_tail); /* after every handler of its turn */
    }
    assert(pending_tail == 3 && idle_turns > 0);

    answer_db(3);
    pump(&app);
    receive(&c);
    assert(strstr(c.rx, "note-1") != NULL && strstr(c.rx, "pong") != NULL);
    const int before = turn_end_calls;
    assert(before > turns);

    app_on_turn_end(&app, NULL, &turn_end_calls);
    assert(app.turn_end_hook == NULL && app.turn_end_udata == NULL);
    assert(app_run_once(&app, 0) == 0);
    assert(turn_end_calls == before);
    teardown(&app);
}

/* A response resumed inside the hook is written by the next turn before it polls, even with nothing else to do;
 * not during the hook's own turn. */
static void test_resume_in_turn_end_hook_goes_out_next_turn(void) {
    App app;
    setup(&app);
    Client c;
    open_client(&app, &c);
    turn_end_calls = 0;
    app_on_turn_end(&app, count_turn_end, &turn_end_calls);
    app_on_turn_end(&app, answer_at_turn_end, NULL); /* replaces the counter's udata too */
    assert(app.turn_end_udata == NULL);

    send_str(&c, "GET /db?id=7 HTTP/1.1\r\nHost: x\r\n\r\n");
    for (int turns = 0; pending_tail == 0 && turns < 100; turns++) {
        assert(app_run_once(&app, 0) >= 0);
    }
    assert(pending_tail == 1 && pending_head == 1); /* deferred and resumed in the same turn */
    receive(&c);
    assert(c.rx_len == 0 && c.conn->deferred != NULL);

    assert(app_run_once(&app, 0) == 0);
    receive(&c);
    assert(strncmp(c.rx, "HTTP/1.1 200 ", 13) == 0 && strstr(c.rx, "from-turn-end") != NULL);
    assert(c.conn->deferred == NULL && is_open(&app, &c));
    app_on_turn_end(&app, NULL, NULL);
    teardown(&app);
}

int main(void) {
    test_deferred_response_waits_for_the_watched_fd();
    test_pipelined_request_waits_behind_deferred();
    test_coalesced_batch_goes_out_before_the_deferred_request();
    test_unsent_batch_tail_is_written_before_the_deferred_response();
    test_client_close_while_deferred();
    test_stale_handle_never_reaches_a_reused_slot();
    test_defer_deadline_answers_504();
    test_app_stop_lets_the_deferred_response_finish();
    test_over_budget_defer_gets_503();
    test_many_concurrent_deferred_requests();
    test_answered_inside_the_handler();
    test_resume_without_sending_answers_500();
    test_watch_fd_interest_and_refusals();
    test_watched_peer_hangup_is_reported();
    test_turn_end_hook_runs_once_per_turn_after_handlers();
    test_resume_in_turn_end_hook_goes_out_next_turn();
    printf("all defer tests passed\n");
    return 0;
}
