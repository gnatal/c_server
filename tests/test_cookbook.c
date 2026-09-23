#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cexpress.h"
#include "arena.h"

Arena test_arena;
char test_arena_buf[64 * 1024];

#include "examples/cookbook.h"

/*
 * Drives every recipe in lib/examples/cookbook.c through the real request path
 * (parse_http_request -> match_route -> dispatch) with a fake Connection, and returns the bytes
 * the engine would write. Caller frees.
 */
static char *fetch(App *app, const char *raw, const size_t raw_len) {
    Connection conn;
    memset(&conn, 0, sizeof(conn));
    /* M1: Connection.arena is a pointer to a shared per-worker Arena (App.arena in the real engine)
     * now, not one embedded per connection - point it at its own local Arena, same buffer as before. */
    Arena conn_arena;
    arena_init(&conn_arena, test_arena_buf, sizeof(test_arena_buf));
    conn.arena = &conn_arena;
    conn.file_fd = -1;
    conn.keep_alive = 1;

    Request req;
    if (parse_http_request(raw, raw_len, &req, &test_arena) != 0) {
        arena_reset(&test_arena);
        char *failed = malloc(12);
        memcpy(failed, "PARSE_ERROR", 12);
        return failed;
    }
    Response res;
    res_init(&res, &conn);
    res.is_head_request = strcmp(req.method, "HEAD") == 0;
    dispatch(app, match_route(app, &req), &req, &res);
    arena_reset(&test_arena);

    size_t out_len = conn.out_len;
    char *out = malloc(out_len + 1);
    memcpy(out, conn.out_buf, out_len);
    /* res_stream (recipe 14): call the producer the way flush_connection does, one STREAM_CHUNK_SIZE
     * turn at a time, until it ends - or, for a paused stream (SSE), until it has nothing more now. */
    static char turn[STREAM_CHUNK_SIZE];
    while (conn.stream_fn != NULL) {
        StreamWriter writer = { turn, 0, sizeof(turn) - 5 };
        const int step = conn.stream_fn(&writer, conn.stream_ctx);
        if (step == STREAM_END) {
            memcpy(turn + writer.len, "0\r\n\r\n", 5);
            writer.len += 5;
        }
        out = realloc(out, out_len + writer.len + 1);
        memcpy(out + out_len, turn, writer.len);
        out_len += writer.len;
        if (step != STREAM_MORE || writer.len == 0) {
            stream_release(&conn); /* frees the ctx, as connection_close would for an open stream */
        }
    }
    out[out_len] = '\0';
    arena_reset(conn.arena);
    return out;
}

static char *get(App *app, const char *request) {
    return fetch(app, request, strlen(request));
}

static const char *body_of(const char *response) {
    const char *sep = strstr(response, "\r\n\r\n");
    return sep != NULL ? sep + 4 : "";
}

static int starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* Sends `request` and asserts the status line and exact body. */
static void expect(App *app, const char *request, const char *status_line, const char *body) {
    char *resp = get(app, request);
    if (!starts_with(resp, status_line) || strcmp(body_of(resp), body) != 0) {
        fprintf(stderr, "request: %s\nexpected: %s | %s\ngot:\n%s\n", request, status_line, body, resp);
        assert(0);
    }
    free(resp);
}

static void expect_header(App *app, const char *request, const char *needle) {
    char *resp = get(app, request);
    if (strstr(resp, needle) == NULL) {
        fprintf(stderr, "request: %s\nmissing: %s\ngot:\n%s\n", request, needle, resp);
        assert(0);
    }
    free(resp);
}

static void test_basic_recipes(App *app) {
    expect(app, "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n", "HTTP/1.1 200 OK", "Hello, world");
    expect(app, "GET /square/7 HTTP/1.1\r\n\r\n", "HTTP/1.1 200 OK", "{\"n\":7,\"square\":49}");
    expect(app, "GET /square/-3 HTTP/1.1\r\n\r\n", "HTTP/1.1 200 OK", "{\"n\":-3,\"square\":9}");
    expect(app, "GET /square/abc HTTP/1.1\r\n\r\n", "HTTP/1.1 400 Bad Request",
           "{\"error\":\"n must be an integer between -1000000 and 1000000\"}");
    expect(app, "GET /greet?name=Ada%20L HTTP/1.1\r\n\r\n", "HTTP/1.1 200 OK", "Hello, Ada L");
    expect(app, "GET /greet HTTP/1.1\r\n\r\n", "HTTP/1.1 200 OK", "Hello, stranger");
    expect(app, "GET /numbers?count=3 HTTP/1.1\r\n\r\n", "HTTP/1.1 200 OK",
           "[{\"i\":0,\"even\":true},{\"i\":1,\"even\":false},{\"i\":2,\"even\":true}]");
    expect(app, "GET /numbers?count=0 HTTP/1.1\r\n\r\n", "HTTP/1.1 200 OK", "[]");
    expect(app, "GET /numbers?count=101 HTTP/1.1\r\n\r\n", "HTTP/1.1 400 Bad Request",
           "{\"error\":\"count must be between 0 and 100\"}");
}

static void test_json_body_recipe(App *app) {
    expect(app,
           "POST /notes HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 20\r\n\r\n{\"title\":\"buy milk\"}",
           "HTTP/1.1 201 Created", "{\"id\":1,\"title\":\"buy milk\"}");
    /* The title is escaped by yyjson, not pasted into the output. */
    expect(app,
           "POST /notes HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 16\r\n\r\n{\"title\":\"a\\\"b\"}",
           "HTTP/1.1 201 Created", "{\"id\":1,\"title\":\"a\\\"b\"}");
    expect(app, "POST /notes HTTP/1.1\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n\r\nhi",
           "HTTP/1.1 415 Unsupported Media Type", "{\"error\":\"expected application/json\"}");
    expect(app, "POST /notes HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}",
           "HTTP/1.1 400 Bad Request", "{\"error\":\"title is required (1-255 characters)\"}");
    char *resp = get(app, "POST /notes HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 1\r\n\r\n{");
    assert(starts_with(resp, "HTTP/1.1 400 Bad Request"));
    free(resp);
}

static void test_middleware_recipes(App *app) {
    /* 6a: app-wide decorator runs on matched routes and on 404s alike. */
    expect_header(app, "GET /hello HTTP/1.1\r\n\r\n", "X-Request-Id: req-1\r\n");
    expect_header(app, "GET /nope HTTP/1.1\r\n\r\n", "X-Request-Id: req-1\r\n");
    expect(app, "GET /nope HTTP/1.1\r\n\r\n", "HTTP/1.1 404 Not Found", "Not Found");

    /* 6b: prefix-scoped guard protects paths under /admin only; chain_error reaches the JSON error handler. */
    expect(app, "GET /admin/stats HTTP/1.1\r\n\r\n", "HTTP/1.1 401 Unauthorized",
           "{\"error\":\"missing or invalid X-Token\"}");
    expect(app, "GET /admin/stats HTTP/1.1\r\nX-Token: wrong\r\n\r\n", "HTTP/1.1 401 Unauthorized",
           "{\"error\":\"missing or invalid X-Token\"}");
    expect(app, "GET /admin/stats HTTP/1.1\r\nx-token: secret\r\n\r\n", "HTTP/1.1 200 OK", "{\"users\":3}");
    expect(app, "GET /hello HTTP/1.1\r\n\r\n", "HTTP/1.1 200 OK", "Hello, world"); /* not under /admin */

    /* 6b: per-route guard protects POST /items only. */
    expect(app, "POST /items HTTP/1.1\r\n\r\n", "HTTP/1.1 401 Unauthorized",
           "{\"error\":\"missing or invalid X-Token\"}");
    expect(app, "POST /items HTTP/1.1\r\nX-Token: secret\r\n\r\n", "HTTP/1.1 201 Created", "{\"created\":true}");

    /* 6c: code after chain_next sees the final status. */
    char *resp = get(app, "GET /nope HTTP/1.1\r\n\r\n");
    free(resp);
    assert(cookbook_last_status_seen() == 404);
    resp = get(app, "GET /hello HTTP/1.1\r\n\r\n");
    free(resp);
    assert(cookbook_last_status_seen() == 200);

    /* Deny by default: right path, wrong method is 405 with Allow, never a silent success. */
    char *r405 = get(app, "PUT /hello HTTP/1.1\r\n\r\n");
    assert(starts_with(r405, "HTTP/1.1 405 Method Not Allowed"));
    assert(strstr(r405, "Allow: GET") != NULL);
    free(r405);
}

static void test_sub_router_recipe(App *app) {
    expect(app, "GET /v1/notes HTTP/1.1\r\n\r\n", "HTTP/1.1 200 OK", "[]");
    expect(app, "GET /v1/notes/7 HTTP/1.1\r\n\r\n", "HTTP/1.1 200 OK", "{\"id\":\"7\"}");
    expect(app, "GET /notes/7 HTTP/1.1\r\n\r\n", "HTTP/1.1 404 Not Found", "Not Found");
}

static void test_cookie_recipes(App *app) {
    expect_header(app, "GET /login HTTP/1.1\r\n\r\n",
                  "Set-Cookie: session=abc123; Path=/; Max-Age=3600; HttpOnly; SameSite=Lax\r\n");
    expect(app, "GET /whoami HTTP/1.1\r\nCookie: theme=dark; session=abc123\r\n\r\n", "HTTP/1.1 200 OK",
           "session=abc123");
    expect(app, "GET /whoami HTTP/1.1\r\n\r\n", "HTTP/1.1 401 Unauthorized", "not logged in");
    expect_header(app, "GET /logout HTTP/1.1\r\n\r\n", "Set-Cookie: session=; Path=/; Max-Age=0\r\n");
}

static void test_form_and_upload_recipes(App *app) {
    expect(app,
           "POST /form HTTP/1.1\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: 18\r\n\r\nname=Ada+L&age=36",
           "HTTP/1.1 200 OK", "Hello, Ada L (36)");
    expect(app, "POST /form HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}",
           "HTTP/1.1 415 Unsupported Media Type", "expected application/x-www-form-urlencoded");

    /* multipart with a binary payload (embedded NUL) - data_len, not strlen, is what counts. */
    const char part_head[] =
        "--B\r\nContent-Disposition: form-data; name=\"file\"; filename=\"a.bin\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n";
    const char payload[] = { 'a', 'b', '\0', 'c', 'd' };
    const char part_tail[] = "\r\n--B--\r\n";
    char body[512];
    size_t n = 0;
    memcpy(body + n, part_head, sizeof(part_head) - 1); n += sizeof(part_head) - 1;
    memcpy(body + n, payload, sizeof(payload));         n += sizeof(payload);
    memcpy(body + n, part_tail, sizeof(part_tail) - 1); n += sizeof(part_tail) - 1;

    char request[1024];
    const int head_len = snprintf(request, sizeof(request),
        "POST /upload HTTP/1.1\r\nContent-Type: multipart/form-data; boundary=B\r\nContent-Length: %zu\r\n\r\n", n);
    memcpy(request + head_len, body, n);
    char *resp = fetch(app, request, (size_t)head_len + n);
    assert(starts_with(resp, "HTTP/1.1 200 OK"));
    assert(strcmp(body_of(resp), "{\"filename\":\"a.bin\",\"bytes\":5}") == 0);
    free(resp);

    expect(app, "POST /upload HTTP/1.1\r\nContent-Type: text/plain\r\nContent-Length: 1\r\n\r\nx",
           "HTTP/1.1 415 Unsupported Media Type", "{\"error\":\"expected multipart/form-data\"}");
}

static void test_redirect_status_and_stream_recipes(App *app) {
    char *resp = get(app, "GET /old HTTP/1.1\r\n\r\n");
    assert(starts_with(resp, "HTTP/1.1 301 Moved Permanently"));
    assert(strstr(resp, "Location: /hello\r\n") != NULL);
    free(resp);

    resp = get(app, "GET /go?next=/home%3Fa%3D1 HTTP/1.1\r\n\r\n");
    assert(starts_with(resp, "HTTP/1.1 302 Found"));
    assert(strstr(resp, "Location: /home?a=1\r\n") != NULL);
    free(resp);
    expect(app, "GET /go?next=//evil.example HTTP/1.1\r\n\r\n", "HTTP/1.1 400 Bad Request",
           "{\"error\":\"next must be a path on this site\"}");
    expect(app, "GET /go?next=https://evil.example HTTP/1.1\r\n\r\n", "HTTP/1.1 400 Bad Request",
           "{\"error\":\"next must be a path on this site\"}");
    /* CR LF smuggled through percent-encoding never reaches the wire. */
    resp = get(app, "GET /go?next=/x%0d%0aSet-Cookie:%20a=b HTTP/1.1\r\n\r\n");
    assert(starts_with(resp, "HTTP/1.1 500 Internal Server Error"));
    assert(strstr(resp, "Set-Cookie") == NULL);
    free(resp);

    resp = get(app, "DELETE /things/5 HTTP/1.1\r\n\r\n");
    assert(starts_with(resp, "HTTP/1.1 204 No Content"));
    /* C3: a 204 carries no framing headers (RFC 9110 8.6) and no default Content-Type. */
    assert(strstr(resp, "Content-Length") == NULL);
    assert(strstr(resp, "Content-Type") == NULL);
    assert(strcmp(body_of(resp), "") == 0);
    free(resp);

    resp = get(app, "POST /things HTTP/1.1\r\n\r\n");
    assert(starts_with(resp, "HTTP/1.1 201 Created"));
    assert(strstr(resp, "Location: /things/9\r\n") != NULL);
    assert(strcmp(body_of(resp), "{\"id\":9}") == 0);
    free(resp);

    /* chunked streaming: three "part N\n" chunks, then the terminating zero chunk. */
    resp = get(app, "GET /stream HTTP/1.1\r\n\r\n");
    assert(starts_with(resp, "HTTP/1.1 200 OK"));
    assert(strstr(resp, "Transfer-Encoding: chunked\r\n") != NULL);
    assert(strcmp(body_of(resp), "7\r\npart 0\n\r\n7\r\npart 1\n\r\n7\r\npart 2\n\r\n0\r\n\r\n") == 0);
    free(resp);

    /* res_stream: a CSV generated by a producer, framed as chunks, ending with the zero chunk. */
    resp = get(app, "GET /export?rows=3 HTTP/1.1\r\n\r\n");
    assert(starts_with(resp, "HTTP/1.1 200 OK"));
    assert(strstr(resp, "Content-Type: text/csv\r\n") != NULL);
    assert(strstr(resp, "Transfer-Encoding: chunked\r\n") != NULL);
    assert(strcmp(body_of(resp), "a\r\nid,square\n\r\n4\r\n0,0\n\r\n4\r\n1,1\n\r\n4\r\n2,4\n\r\n0\r\n\r\n") == 0);
    free(resp);
    resp = get(app, "GET /export?rows=-1 HTTP/1.1\r\n\r\n");
    assert(starts_with(resp, "HTTP/1.1 400 Bad Request"));
    free(resp);

    /* Server-sent events: publish, then a new subscriber replays the log and parks (no zero chunk). */
    const char publish[] = "POST /events HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello";
    resp = fetch(app, publish, sizeof(publish) - 1);
    assert(starts_with(resp, "HTTP/1.1 204 No Content"));
    free(resp);
    const char forged[] = "POST /events HTTP/1.1\r\nContent-Length: 8\r\n\r\nhi\n\ndata:";
    resp = fetch(app, forged, sizeof(forged) - 1);
    assert(starts_with(resp, "HTTP/1.1 400 Bad Request"));
    free(resp);
    resp = get(app, "GET /events HTTP/1.1\r\n\r\n");
    assert(strstr(resp, "Content-Type: text/event-stream\r\n") != NULL);
    assert(strcmp(body_of(resp), "d\r\ndata: hello\n\n\r\n") == 0);
    free(resp);

    /* HEAD on a GET route: same headers, no body (auto-HEAD). */
    resp = get(app, "HEAD /hello HTTP/1.1\r\n\r\n");
    assert(starts_with(resp, "HTTP/1.1 200 OK"));
    assert(strstr(resp, "Content-Length: 12\r\n") != NULL);
    assert(strcmp(body_of(resp), "") == 0);
    free(resp);
}

static void test_worker_hook_recipe(App *app) {
    assert(app->worker_init_hook_count == 1);
    assert(cookbook_worker_resource_opened() == 0); /* registered, not run at registration time */
    cookbook_run_worker_hooks(app);
    assert(cookbook_worker_resource_opened() == 1);
}

int main(void) {
    arena_init(&test_arena, test_arena_buf, sizeof(test_arena_buf));
    static App app; /* App is ~48 KB */
    app_init(&app);
    cookbook_register(&app);

    test_basic_recipes(&app);
    test_json_body_recipe(&app);
    test_middleware_recipes(&app);
    test_sub_router_recipe(&app);
    test_cookie_recipes(&app);
    test_form_and_upload_recipes(&app);
    test_redirect_status_and_stream_recipes(&app);
    test_worker_hook_recipe(&app);

    app_destroy(&app);
    printf("all cookbook tests passed\n");
    return 0;
}
