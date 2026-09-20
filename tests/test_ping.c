#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cexpress.h"

static void handler_ping(const Request *req, Response *res) {
    (void)req;
    res_send(res, "pong");
}

/* Same fake-connection harness as test_cookbook.c: real parse -> route -> dispatch, no sockets. */
static char *fetch(App *app, const char *raw) {
    Connection conn;
    memset(&conn, 0, sizeof(conn));
    conn.file_fd = -1;
    conn.keep_alive = 1;

    Request req;
    assert(parse_http_request(raw, strlen(raw), &req) == 0);
    Response res;
    res_init(&res, &conn);
    res.is_head_request = strcmp(req.method, "HEAD") == 0;
    dispatch(app, match_route(app, &req), &req, &res);
    free(req.body);

    char *out = malloc(conn.out_len + 1);
    memcpy(out, conn.out_buf, conn.out_len + 1); /* out_buf is NUL-terminated */
    free(conn.out_buf);
    return out;
}

int main(void) {
    static App app;
    app_init(&app);
    app_get(&app, "/ping", handler_ping);

    char *resp = fetch(&app, "GET /ping HTTP/1.1\r\nHost: x\r\n\r\n");
    assert(strcmp(resp,
        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 4\r\nConnection: keep-alive\r\n\r\npong") == 0);
    free(resp);

    /* Connection: close is honored (the churn benchmark relies on the server closing after the reply). */
    Connection conn;
    memset(&conn, 0, sizeof(conn));
    conn.file_fd = -1;
    Request req;
    const char *close_req = "GET /ping HTTP/1.1\r\nConnection: close\r\n\r\n";
    assert(parse_http_request(close_req, strlen(close_req), &req) == 0);
    assert(request_wants_close(&req) == 1);
    free(req.body);

    /* HEAD: same headers, no body. */
    resp = fetch(&app, "HEAD /ping HTTP/1.1\r\n\r\n");
    assert(strstr(resp, "Content-Length: 4\r\n") != NULL);
    assert(strcmp(strstr(resp, "\r\n\r\n") + 4, "") == 0);
    free(resp);

    /* Deny by default: other methods are 405 with Allow, other paths 404. */
    resp = fetch(&app, "POST /ping HTTP/1.1\r\nContent-Length: 0\r\n\r\n");
    assert(strncmp(resp, "HTTP/1.1 405 Method Not Allowed", 31) == 0);
    assert(strstr(resp, "Allow: GET\r\n") != NULL);
    free(resp);
    resp = fetch(&app, "GET /ping/extra HTTP/1.1\r\n\r\n");
    assert(strncmp(resp, "HTTP/1.1 404 Not Found", 22) == 0);
    free(resp);

    app_destroy(&app);
    printf("all ping tests passed\n");
    return 0;
}
