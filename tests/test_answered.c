/*
 * T1: "every input is answered or closed", end to end. Each input is written into a socketpair(2) in
 * pieces (whole, byte by byte, every single cut, or random cuts) and driven through the real
 * handle_readable / handle_writable, then the bytes the engine wrote back are compared with a reference
 * model built only from the pure parser (model_run): the same statuses in the same order, a 100 Continue
 * only where the request asked for one (and always for a request left waiting on its body), the
 * connection closed exactly when the model says, "Connection: close" on exactly that last answer, and -
 * when left open - exactly the unanswered tail still buffered. Anything else, or a pump that stops making
 * progress, prints the input and cuts and exits 1.
 *
 *   test_answered            curated corpus with exhaustive cuts + RANDOM_RUNS mutated inputs (make test)
 *   test_answered N [seed]   N mutated inputs, then the corpus (make fuzz, under ASan + UBSan)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include "app_types.h"
#include "arena.h"
#include "event_loop.h"
#include "connection.h"
#include "router.h"
#include "response.h"
#include "middleware.h"
#include "http_parser.h"

#define RANDOM_RUNS 3000
#define MAX_INPUT_LEN 2048
#define MAX_CUTS 8
#define MAX_ANSWERS 64
#define LIMITED_BODY 8          /* app_use_body_limit("/limited") */
#define OUT_CAP (256 * 1024)

/* One answer the model expects, in wire order. */
typedef struct {
    int status;
    int is_head;          /* HEAD: Content-Length is sent, the body is not */
    int may_continue;     /* a 100 Continue ahead of this answer is allowed */
} Answer;

typedef struct {
    Answer answers[MAX_ANSWERS];
    int count;
    int closes;                 /* the engine closes right after answers[count - 1] */
    size_t tail_off;            /* when open: in[tail_off..len) is an unanswered partial request */
    int tail_must_continue;     /* when open: that tail's head asked for 100-continue, so it must have one */
} Model;

static App app;
static Arena model_arena;
static char model_arena_buf[64 * 1024];
static Arena fake_arena;
static char fake_arena_buf[64 * 1024];
static char out[OUT_CAP];
static unsigned long long rng = 0x9E3779B97F4A7C15ULL;
static long runs;

static unsigned r32(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (unsigned)(rng >> 16); }

static void ping_handler(const Request *req, Response *res) { (void)req; res_send(res, "pong"); }
static void item_handler(const Request *req, Response *res) { (void)req; res_send(res, "item"); }
static void echo_len_handler(const Request *req, Response *res) {
    char body[64];
    snprintf(body, sizeof(body), "received %d bytes", req->content_length);
    res_send(res, body);
}

/* ---- failure report ---- */

static const char *cur_in;
static size_t cur_len;
static const size_t *cur_cuts;
static int cur_ncuts;

static void put_escaped(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c == '\r') fputs("\\r", stderr);
        else if (c == '\n') fputs("\\n", stderr);
        else if (c == '"' || c == '\\') fprintf(stderr, "\\%c", c);
        else if (c < 0x20 || c > 0x7e) fprintf(stderr, "\\x%02x\"\"", c);
        else fputc(c, stderr);
    }
}

static void fail(int line, const char *what, size_t out_len) {
    fprintf(stderr, "T1 answered-oracle failed at tests/test_answered.c:%d (%s), run %ld\n  input (%zu bytes): \"",
            line, what, runs, cur_len);
    put_escaped(cur_in, cur_len);
    fputs("\"\n  cuts:", stderr);
    if (cur_ncuts < 0) fputs(" byte-by-byte", stderr);
    for (int i = 0; i < cur_ncuts; i++) fprintf(stderr, " %zu", cur_cuts[i]);
    fputs("\n  engine wrote: \"", stderr);
    put_escaped(out, out_len);
    fputs("\"\n", stderr);
    exit(1);
}

#define CHECK(cond, out_len) do { if (!(cond)) fail(__LINE__, #cond, (out_len)); } while (0)

/* ---- reference model: the whole input, parsed as one buffer by the pure functions ---- */

static int reject_status(const int parse_status, const Request *req) {
    /* Same mapping as connection.c's serve_buffered_requests. */
    return parse_status == -2 ? 414 : req->content_length == -2 ? 413 : req->content_length == -3 ? 501 : 400;
}

/* 1 when p[0..n) holds a blank line ('\n' then "\n" or "\r\n"): the end of any head. Parser-independent. */
static int has_blank_line(const char *p, const size_t n) {
    for (size_t i = 0; i + 1 < n; i++) {
        if (p[i] == '\n' && (p[i + 1] == '\n' || (p[i + 1] == '\r' && i + 2 < n && p[i + 2] == '\n'))) return 1;
    }
    return 0;
}

static void model_run(const char *in, const size_t len, Model *m) {
    size_t off = 0;
    m->count = 0;
    m->closes = 0;
    m->tail_must_continue = 0;
    for (;;) {
        m->tail_off = off;
        if (off == len) return;
        CHECK(m->count < MAX_ANSWERS, 0);
        const size_t avail = len - off;
        char *p = malloc(avail); /* exact size: ASan sees any read past the request */
        CHECK(p != NULL, 0);
        memcpy(p, in + off, avail);
        ParsedHead h;
        parse_request_head(p, avail, &h);
        Answer *a = &m->answers[m->count];
        a->is_head = 0;
        a->may_continue = request_head_expects_continue(&h);

        /* S4: a declared Content-Length over the route's limit is refused once the head is in. */
        if (h.header_len > 0 && !h.chunked && h.content_length >= 0) {
            char path[256];
            const size_t n = h.path_len < sizeof(path) - 1 ? h.path_len : sizeof(path) - 1;
            memcpy(path, h.path, n);
            path[n] = '\0';
            if ((size_t)h.content_length > app_body_limit_for_path(&app, path)) {
                a->status = 413;
                a->may_continue = 0;
                m->count++;
                m->closes = 1;
                free(p);
                return;
            }
        }

        ChunkScanState cs = {0};
        if (!request_head_is_complete(&h, p, avail, &cs)) {
            /* The model trusts the parser's "wait" only where waiting is right: no head end yet, or a
             * well-framed head whose body is genuinely short. Anything else would be held unanswered. */
            if (h.header_len == 0) {
                CHECK(!has_blank_line(p, avail), 0);
            } else if (h.chunked) {
                size_t decoded;
                CHECK(chunked_body_scan(p + h.header_len, avail - h.header_len, MAX_BODY_SIZE, &decoded) == 0, 0);
            } else {
                CHECK(h.content_length > 0 && avail - h.header_len < (size_t)h.content_length, 0);
            }
            m->tail_must_continue = a->may_continue;
            free(p);
            return;
        }
        Request req;
        const int st = parse_http_request(p, avail, &req, &model_arena);
        if (st != 0) {
            a->status = reject_status(st, &req);
            m->count++;
            m->closes = 1;
            arena_reset(&model_arena);
            free(p);
            return;
        }
        Connection fake;
        memset(&fake, 0, sizeof(fake));
        fake.arena = &fake_arena;
        fake.file_fd = -1;
        fake.keep_alive = 1;
        Response res;
        res_init(&res, &fake);
        res.is_head_request = strcmp(req.method, "HEAD") == 0;
        a->is_head = res.is_head_request;
        dispatch(&app, match_route(&app, &req), &req, &res);
        a->status = res.status;
        const int closes = request_wants_close(&req);
        off += request_wire_len(&h, &cs);
        CHECK(off <= len, 0);
        m->count++;
        arena_reset(&model_arena);
        arena_reset(&fake_arena);
        free(p);
        if (closes) {
            m->closes = 1;
            m->tail_off = off;
            return;
        }
    }
}

/* ---- driving the real engine ---- */

static void drain(const int peer, size_t *out_len) {
    for (;;) {
        CHECK(*out_len < OUT_CAP, *out_len);
        const ssize_t n = read(peer, out + *out_len, OUT_CAP - *out_len);
        if (n <= 0) return;
        *out_len += (size_t)n;
    }
}

/* Plays a level-triggered event loop until nothing is ready: the peer is always drained, so the socket
 * is writable whenever the engine watches for that (a pending response, or pipelined requests parked
 * behind MAX_PIPELINED_PER_EVENT), and readable while unread bytes wait and it watches for reads. A
 * bounded number of turns - a stuck connection is a failure, not a hang. */
static void pump(const int fd, const int peer, size_t *out_len) {
    for (int turn = 0;; turn++) {
        CHECK(turn < 4096, *out_len);
        drain(peer, out_len);
        Connection *c = app.connections[fd];
        if (c == NULL) return;
        int unread = 0;
        CHECK(ioctl(fd, FIONREAD, &unread) == 0, *out_len);
        if (c->events_watched & EVENT_WRITE) {
            handle_writable(&app, c);
        } else if ((c->events_watched & EVENT_READ) && unread > 0) {
            handle_readable(&app, c);
        } else {
            return;
        }
    }
}

static const char *find(const char *hay, const size_t hay_len, const char *needle) {
    const size_t n = strlen(needle);
    for (size_t i = 0; i + n <= hay_len; i++) {
        if (strncasecmp(hay + i, needle, n) == 0) return hay + i;
    }
    return NULL;
}

/* cuts[0..ncuts) ascending, each in (0, len); ncuts == -1 means one byte per write. */
static void run_split(const char *in, const size_t len, const size_t *cuts, const int ncuts, const Model *m) {
    runs++;
    cur_in = in;
    cur_len = len;
    cur_cuts = cuts;
    cur_ncuts = ncuts;
    size_t out_len = 0;

    int fds[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, 0);
    CHECK(set_nonblocking(fds[0]) == 0 && set_nonblocking(fds[1]) == 0, 0);
    Connection *conn = connection_create(&app, fds[0]);
    CHECK(conn != NULL && fds[0] < app.connections_cap, 0);
    app.connections[fds[0]] = conn;
    app.open_connections++; /* paired with connection_close's decrement */
    CHECK(event_loop_watch_read(&app, fds[0], conn) == 0, 0);

    const int pieces = ncuts < 0 ? (int)len : ncuts + 1;
    for (int i = 0; i < pieces; i++) {
        const size_t from = ncuts < 0 ? (size_t)i : i == 0 ? 0 : cuts[i - 1];
        const size_t to = ncuts < 0 ? (size_t)i + 1 : i == ncuts ? len : cuts[i];
        if (app.connections[fds[0]] == NULL) break; /* closed early: the rest is never read */
        CHECK(write(fds[1], in + from, to - from) == (ssize_t)(to - from), out_len);
        pump(fds[0], fds[1], &out_len);
    }
    pump(fds[0], fds[1], &out_len);

    /* Walk the engine's output against the model. */
    size_t pos = 0;
    int i = 0;
    int continued = 0;
    static const char CONTINUE[] = "HTTP/1.1 100 Continue\r\n\r\n";
    while (pos < out_len) {
        if (out_len - pos >= sizeof(CONTINUE) - 1 && memcmp(out + pos, CONTINUE, sizeof(CONTINUE) - 1) == 0) {
            const int allowed = i < m->count ? m->answers[i].may_continue : (!m->closes && m->tail_must_continue);
            CHECK(allowed && !continued, out_len); /* only where asked, at most once per request */
            continued = 1;
            pos += sizeof(CONTINUE) - 1;
            continue;
        }
        CHECK(i < m->count, out_len); /* an answer the model has no request for */
        CHECK(out_len - pos > 12 && memcmp(out + pos, "HTTP/1.1 ", 9) == 0, out_len);
        const int status = atoi(out + pos + 9);
        CHECK(status == m->answers[i].status, out_len);
        const char *head_end = find(out + pos, out_len - pos, "\r\n\r\n");
        CHECK(head_end != NULL, out_len);
        const size_t head_len = (size_t)(head_end + 4 - (out + pos));
        const char *cl = find(out + pos, head_len, "\r\nContent-Length: ");
        const size_t body = cl != NULL && !m->answers[i].is_head ? strtoul(cl + 18, NULL, 10) : 0;
        CHECK(pos + head_len + body <= out_len, out_len);
        const int last_and_closes = i == m->count - 1 && m->closes;
        CHECK((find(out + pos, head_len, "\r\nConnection: close\r\n") != NULL) == last_and_closes, out_len);
        pos += head_len + body;
        i++;
        continued = 0;
    }
    CHECK(i == m->count, out_len); /* every request the model answers was answered */

    Connection *c = app.connections[fds[0]];
    CHECK((c == NULL) == m->closes, out_len);
    char probe;
    const ssize_t eof = read(fds[1], &probe, 1);
    CHECK(m->closes ? eof == 0 : eof < 0, out_len); /* the peer sees the close, and only then */
    if (c != NULL) {
        const size_t have = c->in_buf != NULL ? c->in_len - c->in_off : 0;
        CHECK(have == len - m->tail_off, out_len);
        CHECK(have == 0 || memcmp(c->in_buf + c->in_off, in + m->tail_off, have) == 0, out_len);
        if (m->tail_must_continue) CHECK(continued, out_len); /* C1: a body the engine waits on is invited */
        connection_close(&app, c);
    }
    close(fds[1]);
}

/* Every split strategy for one input: whole, byte by byte, and every single cut. */
static void run_exhaustive(const char *in, const size_t len) {
    Model m;
    cur_in = in;
    cur_len = len;
    cur_ncuts = 0;
    model_run(in, len, &m);
    run_split(in, len, NULL, 0, &m);
    run_split(in, len, NULL, -1, &m);
    for (size_t k = 1; k < len; k++) {
        run_split(in, len, &k, 1, &m);
    }
}

static void run_random_cuts(const char *in, const size_t len) {
    Model m;
    cur_in = in;
    cur_len = len;
    cur_ncuts = 0;
    model_run(in, len, &m);
    for (int round = 0; round < 3; round++) {
        size_t cuts[MAX_CUTS];
        int n = 0;
        const int want = len > 1 ? (int)(r32() % MAX_CUTS) : 0;
        for (int j = 0; j < want; j++) {
            const size_t k = 1 + r32() % (len - 1);
            int dup = 0;
            for (int q = 0; q < n; q++) dup |= cuts[q] == k;
            if (!dup) cuts[n++] = k;
        }
        for (int a = 1; a < n; a++) { /* insertion sort, n <= MAX_CUTS */
            const size_t v = cuts[a];
            int b = a - 1;
            while (b >= 0 && cuts[b] > v) { cuts[b + 1] = cuts[b]; b--; }
            cuts[b + 1] = v;
        }
        run_split(in, len, cuts, n, &m);
    }
}

/* ---- inputs ---- */

static const char *seeds[] = {
    "GET /ping HTTP/1.1\r\nHost: x\r\n\r\n",
    "HEAD /ping HTTP/1.1\r\n\r\n",
    "POST /echo HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello",
    "POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nWiki\r\n5;x=1\r\npedia\r\n0\r\nT: v\r\n\r\n",
    "POST /echo HTTP/1.1\r\nExpect: 100-continue\r\nContent-Length: 3\r\n\r\nabc",
    "POST /echo HTTP/1.1\r\nExpect: 100-continue\r\nTransfer-Encoding: chunked\r\n\r\n1\r\nz\r\n0\r\n\r\n",
    "POST /limited HTTP/1.1\r\nContent-Length: 9\r\n\r\n123456789",
    "POST /limited HTTP/1.1\r\nExpect: 100-continue\r\nContent-Length: 2\r\n\r\nok",
    "GET /items/42?a=1&b=%41 HTTP/1.1\r\nCookie: a=1\r\n\r\n",
    "GET /nope HTTP/1.1\r\n\r\n",
    "DELETE /ping HTTP/1.1\r\n\r\n",
    "OPTIONS /ping HTTP/1.1\r\n\r\n",
    "GET /ping HTTP/1.0\r\n\r\n",
    "GET /ping HTTP/1.0\r\nConnection: keep-alive\r\n\r\n",
    "GET /ping HTTP/1.1\r\nConnection: close\r\n\r\n",
    "\r\nGET /ping HTTP/1.1\r\n\r\n",
    "GET / X\r\n\r\n",
    "GET /ping HTTP\r\n\r\n",
    "GET /ping\r\n\r\n",
    "hello there\r\n\r\n",
    "GET /%00 HTTP/1.1\r\n\r\n",
    "GET /ping HTTP/1.1\r\nHost: x\n\r\n",
    "POST /echo HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\n",
    "POST /echo HTTP/1.1\r\nContent-Length: 99999999\r\n\r\n",
    "POST /echo HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nab",
    "POST /echo HTTP/1.1\r\nContent-Length: 2\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
    "POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n\r\n",
};
#define NSEEDS (sizeof(seeds) / sizeof(seeds[0]))

/* Hand-built inputs beyond single seeds: pipelines, garbage after good requests, and partial tails. */
static void run_corpus(void) {
    for (size_t i = 0; i < NSEEDS; i++) {
        run_exhaustive(seeds[i], strlen(seeds[i]));
    }
    static char buf[MAX_INPUT_LEN];
    const char *pipelines[][4] = {
        {"GET /ping HTTP/1.1\r\n\r\n", "POST /echo HTTP/1.1\r\nContent-Length: 2\r\n\r\nhi", "HEAD /ping HTTP/1.1\r\n\r\n", NULL},
        {"GET /ping HTTP/1.1\r\n\r\n", "GET / X\r\n\r\n", "GET /ping HTTP/1.1\r\n\r\n", NULL},
        {"POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nab\r\n0\r\n\r\n", "GET /nope HTTP/1.1\r\n\r\n", NULL, NULL},
        {"GET /ping HTTP/1.1\r\n\r\n", "POST /echo HTTP/1.1\r\nExpect: 100-continue\r\nContent-Length: 10\r\n\r\nabc", NULL, NULL},
        {"GET /ping HTTP/1.1\r\n\r\n", "POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nab", NULL, NULL},
        {"GET /ping HTTP/1.1\r\n\r\n", "GET /ping HTTP/1.1\r\nHost: x\r\n", NULL, NULL},
        {"GET /ping HTTP/1.1\r\nConnection: close\r\n\r\n", "GET /ping HTTP/1.1\r\n\r\n", NULL, NULL},
        {"GET /ping HTTP/1.1\r\n\r\n", "POST /limited HTTP/1.1\r\nContent-Length: 100\r\n\r\n", NULL, NULL},
    };
    for (size_t p = 0; p < sizeof(pipelines) / sizeof(pipelines[0]); p++) {
        size_t len = 0;
        for (int j = 0; j < 4 && pipelines[p][j] != NULL; j++) {
            const size_t n = strlen(pipelines[p][j]);
            memcpy(buf + len, pipelines[p][j], n);
            len += n;
        }
        run_exhaustive(buf, len);
    }

    /* More pipelined requests than MAX_PIPELINED_PER_EVENT: the rest is served on write readiness. */
    size_t len = 0;
    for (int j = 0; j < MAX_PIPELINED_PER_EVENT + 4; j++) {
        static const char REQ[] = "GET /ping HTTP/1.1\r\n\r\n";
        memcpy(buf + len, REQ, sizeof(REQ) - 1);
        len += sizeof(REQ) - 1;
    }
    run_exhaustive(buf, len);

    /* A request-target over the 255-byte path buffer: 414. */
    len = (size_t)snprintf(buf, sizeof(buf), "GET /");
    memset(buf + len, 'a', 300);
    len += 300;
    len += (size_t)snprintf(buf + len, sizeof(buf) - len, " HTTP/1.1\r\n\r\n");
    run_exhaustive(buf, len);
}

/* 1-3 seeds back to back, then the byte mutations fuzz_parser.c uses (sometimes none, so valid
 * pipelines stay common). */
static size_t random_input(char *buf) {
    size_t len = 0;
    const int parts = 1 + (int)(r32() % 3);
    for (int j = 0; j < parts; j++) {
        const char *s = seeds[r32() % NSEEDS];
        const size_t n = strlen(s);
        if (len + n > MAX_INPUT_LEN / 2) break;
        memcpy(buf + len, s, n);
        len += n;
    }
    const int muts = (int)(r32() % 5);
    for (int k = 0; k < muts; k++) {
        switch (r32() % 5) {
        case 0: if (len) buf[r32() % len] = (char)r32(); break;
        case 1: if (len > 1) len = 1 + r32() % (len - 1); break;
        case 2: if (len < MAX_INPUT_LEN) { const size_t p = len ? r32() % len : 0; memmove(buf + p + 1, buf + p, len - p); buf[p] = "\r\n: ;=%&?/*0"[r32() % 12]; len++; } break;
        case 3: if (len > 2) { const size_t p = r32() % (len - 1); memmove(buf + p, buf + p + 1, len - p - 1); len--; } break;
        default: if (len) buf[r32() % len] = "\r\n\0:"[r32() % 4]; break;
        }
    }
    return len;
}

int main(int argc, char **argv) {
    const long random_runs = argc > 1 ? atol(argv[1]) : RANDOM_RUNS;
    if (argc > 2) rng ^= strtoull(argv[2], NULL, 10) * 0x2545F4914F6CDD1DULL;
    arena_init(&model_arena, model_arena_buf, sizeof(model_arena_buf));
    arena_init(&fake_arena, fake_arena_buf, sizeof(fake_arena_buf));
    app_init(&app);
    if (event_loop_init(&app) != 0) {
        fprintf(stderr, "event_loop_init failed\n");
        return 1;
    }
    app_get(&app, "/ping", ping_handler);
    app_get(&app, "/items/:id", item_handler);
    app_post(&app, "/echo", echo_len_handler);
    app_post(&app, "/limited", echo_len_handler);
    app_use_body_limit(&app, "/limited", LIMITED_BODY);

    run_corpus();
    const long corpus_runs = runs;
    static char buf[MAX_INPUT_LEN];
    for (long r = 0; r < random_runs; r++) {
        const size_t len = random_input(buf);
        run_random_cuts(buf, len);
    }
    printf("answered ok: %ld corpus runs, %ld random inputs (%ld runs)\n", corpus_runs, random_runs,
           runs - corpus_runs);
    app_free_routes(&app);
    app_destroy(&app);
    return 0;
}
