/*
 * app_use_body_limit: the limit is looked up on the canonical path (request_target_path) and applies
 * to chunked bodies too. Pure checks of request_target_path / app_body_limit_for_target, then the
 * engine end to end over a socketpair(2) peer (handle_readable called directly, no real listener),
 * the same harness as test_pipelining.c.
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

#define UPLOAD_LIMIT 16

static void echo_len_handler(const Request *req, Response *res) {
    char body[64];
    snprintf(body, sizeof(body), "received %d bytes", req->content_length);
    res_status(res, 200);
    res_send(res, body);
}

/* ---- pure ---- */

static void test_request_target_path(void) {
    char out[256];
    assert(request_target_path("/upload?x=1", 11, out, sizeof(out)) == 0);
    assert(strcmp(out, "/upload") == 0);
    assert(request_target_path("//upload//x", 11, out, sizeof(out)) == 0);
    assert(strcmp(out, "/upload/x") == 0);
    assert(request_target_path("/%75pload", 9, out, sizeof(out)) == 0);
    assert(strcmp(out, "/upload") == 0);
    assert(request_target_path("/", 1, out, sizeof(out)) == 0);
    assert(strcmp(out, "/") == 0);
    /* the length bounds the target: bytes past it (e.g. " HTTP/1.1" in a raw buffer) are ignored */
    assert(request_target_path("/upload HTTP/1.1", 7, out, sizeof(out)) == 0);
    assert(strcmp(out, "/upload") == 0);

    assert(request_target_path("/a%2Fb", 6, out, sizeof(out)) == -4);
    assert(request_target_path("/a/../b", 7, out, sizeof(out)) == -4);
    assert(request_target_path("/x%00y", 6, out, sizeof(out)) == -4);
    assert(request_target_path("upload", 6, out, sizeof(out)) == -4);

    /* path part must fit; a long query does not count against it */
    char long_target[300];
    long_target[0] = '/';
    memset(long_target + 1, 'a', sizeof(long_target) - 1);
    assert(request_target_path(long_target, sizeof(long_target), out, sizeof(out)) == -2);
    char long_query[300] = "/upload?";
    memset(long_query + 8, 'q', sizeof(long_query) - 8);
    assert(request_target_path(long_query, sizeof(long_query), out, sizeof(out)) == 0);
    assert(strcmp(out, "/upload") == 0);
}

static void test_body_limit_for_target(void) {
    App app;
    app_init(&app);
    app_use_body_limit(&app, "/upload", UPLOAD_LIMIT);

    const char *covered[] = {"/upload", "/upload?x=1", "//upload", "/%75pload", "/upload/", "//upload//a?b"};
    for (size_t i = 0; i < sizeof(covered) / sizeof(covered[0]); i++) {
        assert(app_body_limit_for_target(&app, covered[i], strlen(covered[i])) == UPLOAD_LIMIT);
    }
    assert(app_body_limit_for_target(&app, "/uploads", 8) == MAX_BODY_SIZE);
    assert(app_body_limit_for_target(&app, "/other?p=/upload", 16) == MAX_BODY_SIZE);
    /* rejected targets fall back to the global cap: the full parse answers them with 400 */
    assert(app_body_limit_for_target(&app, "/%2Fupload", 10) == MAX_BODY_SIZE);

    app_destroy(&app);
}

/* ---- engine ---- */

static void setup(App *app, int fds[2], Connection **conn) {
    app_init(app);
    assert(event_loop_init(app) == 0);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    assert(set_nonblocking(fds[0]) == 0);
    assert(set_nonblocking(fds[1]) == 0);
    *conn = connection_create(app, fds[0]);
    assert(*conn != NULL);
    app->connections[fds[0]] = *conn;
    assert(event_loop_watch_read(app, fds[0], *conn) == 0);

    app_post(app, "/upload", echo_len_handler);
    app_post(app, "/other", echo_len_handler);
    app_use_body_limit(app, "/upload", UPLOAD_LIMIT);
}

static void teardown(App *app, int fds[2]) {
    close(fds[1]);
    app_destroy(app); /* closes and frees whatever connection is still tracked */
}

/* Writes data to the peer as fast as the socket takes it, running handle_readable after every
 * write, until it is all sent or the server closed the connection. Returns 1 if the connection is
 * still open afterwards. */
static int pump(App *app, const int fds[2], const char *data, const size_t len) {
    size_t sent = 0;
    while (app->connections[fds[0]] != NULL) {
        if (sent < len) {
            const ssize_t n = write(fds[1], data + sent, len - sent);
            if (n > 0) {
                sent += (size_t)n;
            }
        }
        handle_readable(app, app->connections[fds[0]]);
        if (sent == len) {
            break;
        }
    }
    return app->connections[fds[0]] != NULL;
}

/* Everything the server has written so far (non-blocking peer), NUL-terminated. */
static void read_all(const int fd, char *buf, const size_t cap) {
    size_t used = 0;
    ssize_t n;
    while (used + 1 < cap && (n = read(fd, buf + used, cap - 1 - used)) > 0) {
        used += (size_t)n;
    }
    buf[used] = '\0';
}

/* "<n>\r\n<n bytes>\r\n" repeated to `total` body bytes in chunks of `chunk`, then the last chunk. */
static size_t build_chunked(char *out, const size_t cap, const size_t total, const size_t chunk) {
    size_t len = 0;
    for (size_t done = 0; done < total; done += chunk) {
        const size_t this_chunk = total - done < chunk ? total - done : chunk;
        len += (size_t)snprintf(out + len, cap - len, "%zx\r\n", this_chunk);
        assert(len + this_chunk + 2 < cap);
        memset(out + len, 'b', this_chunk);
        len += this_chunk;
        memcpy(out + len, "\r\n", 2);
        len += 2;
    }
    len += (size_t)snprintf(out + len, cap - len, "0\r\n\r\n");
    assert(len < cap);
    return len;
}

/* Sends head + body on a fresh connection; returns 1 if it stayed open, the response in resp. */
static int exchange(const char *head, const char *body, const size_t body_len, char *resp, const size_t cap) {
    App app;
    int fds[2];
    Connection *conn;
    setup(&app, fds, &conn);
    const size_t head_len = strlen(head);
    char *req = malloc(head_len + body_len);
    assert(req != NULL);
    memcpy(req, head, head_len);
    memcpy(req + head_len, body, body_len);
    const int open = pump(&app, fds, req, head_len + body_len);
    free(req);
    read_all(fds[1], resp, cap);
    teardown(&app, fds);
    return open;
}

/* The four shapes that used to fall back to the global 10 MiB cap, each with a 5,000-byte body. */
static void test_bypass_shapes_get_413(void) {
    static char body[5000];
    memset(body, 'b', sizeof(body));
    static char chunked[8192];
    const size_t chunked_len = build_chunked(chunked, sizeof(chunked), sizeof(body), 1000);
    char resp[1024];

    const char *cl_heads[] = {
        "POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: 5000\r\n\r\n",
        "POST /upload?x=1 HTTP/1.1\r\nHost: x\r\nContent-Length: 5000\r\n\r\n",
        "POST //upload HTTP/1.1\r\nHost: x\r\nContent-Length: 5000\r\n\r\n",
        "POST /%75pload HTTP/1.1\r\nHost: x\r\nContent-Length: 5000\r\n\r\n",
    };
    for (size_t i = 0; i < sizeof(cl_heads) / sizeof(cl_heads[0]); i++) {
        assert(!exchange(cl_heads[i], body, sizeof(body), resp, sizeof(resp)));
        assert(strncmp(resp, "HTTP/1.1 413 ", 13) == 0);
    }

    const char *chunked_heads[] = {
        "POST /upload HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n",
        "POST //upload?x=1 HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n",
    };
    for (size_t i = 0; i < sizeof(chunked_heads) / sizeof(chunked_heads[0]); i++) {
        assert(!exchange(chunked_heads[i], chunked, chunked_len, resp, sizeof(resp)));
        assert(strncmp(resp, "HTTP/1.1 413 ", 13) == 0);
    }
}

/* Within the limit, the same shapes are served: the limit tightens, it never breaks valid requests.
 * A chunked body of exactly the limit counts decoded bytes, not its (larger) framing. */
static void test_within_limit_is_served(void) {
    char resp[1024];
    assert(exchange("POST /upload?x=1 HTTP/1.1\r\nHost: x\r\nContent-Length: 16\r\n\r\n", "0123456789abcdef", 16, resp,
                    sizeof(resp)));
    assert(strncmp(resp, "HTTP/1.1 200 ", 13) == 0);
    assert(strstr(resp, "received 16 bytes") != NULL);

    char chunked[256];
    const size_t chunked_len = build_chunked(chunked, sizeof(chunked), UPLOAD_LIMIT, 1);
    assert(exchange("POST //upload HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n", chunked, chunked_len, resp,
                    sizeof(resp)));
    assert(strncmp(resp, "HTTP/1.1 200 ", 13) == 0);
    assert(strstr(resp, "received 16 bytes") != NULL);
}

/* One chunk declared far over the limit whose data never finishes: no chunk ever validates, so it
 * is the raw-size cap in grow_in_buf (the route's limit, not MAX_BODY_SIZE) that refuses it once the
 * first read buffer is full - the server must not keep growing in_buf toward 10 MiB. */
static void test_unfinished_huge_chunk_hits_raw_cap(void) {
    static char body[3 * BUF_SIZE];
    const int prefix = snprintf(body, sizeof(body), "100000\r\n");
    memset(body + prefix, 'b', sizeof(body) - (size_t)prefix);
    char resp[1024];
    assert(!exchange("POST /upload HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n", body, sizeof(body), resp,
                     sizeof(resp)));
    assert(strncmp(resp, "HTTP/1.1 413 ", 13) == 0);
}

/* The limit is per request: a keep-alive connection serves a large chunked body on an unlimited
 * route, then still refuses the next request's body on the limited one. */
static void test_limit_is_rechecked_per_request(void) {
    App app;
    int fds[2];
    Connection *conn;
    setup(&app, fds, &conn);

    static char chunked[8192];
    const size_t chunked_len = build_chunked(chunked, sizeof(chunked), 5000, 1000);
    static char req[16384];
    size_t len = (size_t)snprintf(req, sizeof(req), "POST /other HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n");
    memcpy(req + len, chunked, chunked_len);
    len += chunked_len;
    assert(pump(&app, fds, req, len));
    char resp[1024];
    read_all(fds[1], resp, sizeof(resp));
    assert(strncmp(resp, "HTTP/1.1 200 ", 13) == 0);
    assert(strstr(resp, "received 5000 bytes") != NULL);

    len = (size_t)snprintf(req, sizeof(req), "POST /upload HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n");
    memcpy(req + len, chunked, chunked_len);
    len += chunked_len;
    assert(!pump(&app, fds, req, len));
    read_all(fds[1], resp, sizeof(resp));
    assert(strncmp(resp, "HTTP/1.1 413 ", 13) == 0);

    teardown(&app, fds);
}

int main(void) {
    test_request_target_path();
    test_body_limit_for_target();
    test_bypass_shapes_get_413();
    test_within_limit_is_served();
    test_unfinished_huge_chunk_hits_raw_cap();
    test_limit_is_rechecked_per_request();
    printf("all body limit tests passed\n");
    return 0;
}
