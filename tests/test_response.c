#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "app_types.h"
#include "response.h"

#include "arena.h"
#include "http_parser.h"

static Connection *make_conn(void) {
    Connection *conn = calloc(1, sizeof(Connection));
    /* Connection.arena is a pointer to a shared per-worker Arena (App.arena in the real engine)
     * now, not one embedded per connection - malloc a standalone Arena for this fixture to point at. */
    Arena *arena = malloc(sizeof(Arena));
    arena_init(arena, malloc(64 * 1024), 64 * 1024);
    conn->arena = arena;
    conn->keep_alive = 1;
    conn->file_fd = -1;
    return conn;
}

/* every head carries "Date: <IMF-fixdate>\r\n" right after the status line. Asserts that, then returns a
 * malloc'd copy of the response without that line (caller frees), so exact-byte checks stay clock-independent. */
static char *without_date(const char *resp, size_t len, size_t *out_len) {
    const char *status_end = memchr(resp, '\n', len);
    assert(status_end != NULL);
    const char *date = status_end + 1;
    const size_t date_line = strlen("Date: ") + HTTP_DATE_LEN + 2;
    assert((size_t)(resp + len - date) >= date_line);
    assert(memcmp(date, "Date: ", 6) == 0);
    assert(memcmp(date + 6 + HTTP_DATE_LEN - 4, " GMT\r\n", 6) == 0);
    const size_t head_part = (size_t)(date - resp);
    char *out = malloc(len - date_line + 1);
    assert(out != NULL);
    memcpy(out, resp, head_part);
    memcpy(out + head_part, date + date_line, len - head_part - date_line);
    *out_len = len - date_line;
    out[*out_len] = '\0';
    return out;
}

static void free_conn(Connection *conn) {
    void *buf = conn->arena->buf;
    arena_reset(conn->arena);
    free(buf);
    free(conn->arena);
    free(conn);
}

static void test_custom_header_is_sent(void) {
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    res_status(&res, 200);
    res_set_header(&res, "X-Custom", "hello");
    res_send(&res, "body");

    assert(conn->out_buf != NULL);
    assert(strstr(conn->out_buf, "X-Custom: hello\r\n") != NULL);

    free_conn(conn);
}

static void test_repeated_set_header_overwrites_case_insensitively(void) {
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    res_status(&res, 200);
    res_set_header(&res, "X-Custom", "first");
    res_set_header(&res, "x-custom", "second");
    res_send(&res, "body");

    assert(res.header_count == 1);
    assert(strstr(conn->out_buf, "second") != NULL);
    /* Only the final value should appear - not both. */
    assert(strstr(conn->out_buf, "first") == NULL);

    free_conn(conn);
}

static void test_reserved_headers_are_rejected(void) {
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    res_status(&res, 200);
    res_set_header(&res, "Content-Length", "999");
    res_set_header(&res, "Connection", "close");

    /* Rejected outright - never stored as custom headers. */
    assert(res.header_count == 0);

    res_send(&res, "body");

    /* The response layer's own computed values still win. */
    assert(strstr(conn->out_buf, "Content-Length: 4\r\n") != NULL);
    assert(strstr(conn->out_buf, "Connection: keep-alive\r\n") != NULL);
    assert(strstr(conn->out_buf, "Content-Length: 999") == NULL);
    assert(strstr(conn->out_buf, "Connection: close") == NULL);

    free_conn(conn);
}

static void test_custom_content_type_overrides_default(void) {
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    res_status(&res, 200);
    res_set_header(&res, "Content-Type", "text/html");
    res_send(&res, "<p>hi</p>");

    assert(strstr(conn->out_buf, "Content-Type: text/html\r\n") != NULL);
    /* The default text/plain content type must not also appear. */
    assert(strstr(conn->out_buf, "Content-Type: text/plain") == NULL);

    free_conn(conn);
}

static void test_max_response_headers_enforced(void) {
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    res_status(&res, 200);
    for (int i = 0; i < MAX_RESPONSE_HEADERS + 4; i++) {
        char name[32];
        snprintf(name, sizeof(name), "X-Header-%d", i);
        res_set_header(&res, name, "v");
    }

    /* Extras beyond the fixed cap are dropped, not overflowed into. */
    assert(res.header_count == MAX_RESPONSE_HEADERS);

    res_send(&res, "body");
    assert(conn->out_buf != NULL);

    free_conn(conn);
}

static void test_head_response_omits_body_but_keeps_content_length(void) {
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0, .is_head_request = 1 };

    res_status(&res, 200);
    res_send(&res, "hello world");

    assert(conn->out_buf != NULL);
    /* Content-Length still reflects the body a GET would have sent (RFC
     * 7231 4.3.2)... */
    assert(strstr(conn->out_buf, "Content-Length: 11\r\n") != NULL);
    /* ...but out_buf itself stops right after the headers - out_len equals
     * exactly the header block's length, no body bytes appended past it
     * (checked by length rather than strstr's absence, since out_buf past
     * out_len is unallocated memory, not a NUL-terminated C string). */
    const char *header_end = strstr(conn->out_buf, "\r\n\r\n");
    assert(header_end != NULL);
    assert(conn->out_len == (size_t)(header_end + 4 - conn->out_buf));

    free_conn(conn);
}

static void test_set_cookie_defaults(void) {
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    res_status(&res, 200);
    res_set_cookie(&res, "session", "abc123", NULL);
    res_send(&res, "body");

    assert(conn->out_buf != NULL);
    /* No options: session cookie, Path=/, no Domain/Max-Age/HttpOnly/
     * Secure/SameSite. */
    assert(strstr(conn->out_buf, "Set-Cookie: session=abc123; Path=/\r\n") != NULL);
    assert(strstr(conn->out_buf, "Max-Age") == NULL);
    assert(strstr(conn->out_buf, "HttpOnly") == NULL);

    free_conn(conn);
}

static void test_set_cookie_with_options(void) {
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };
    const CookieOptions options = { .max_age = 3600, .path = "/api", .domain = "example.com",
                                     .http_only = 1, .secure = 1, .same_site = COOKIE_SAMESITE_STRICT };

    res_status(&res, 200);
    res_set_cookie(&res, "session", "abc123", &options);
    res_send(&res, "body");

    assert(conn->out_buf != NULL);
    assert(strstr(conn->out_buf,
        "Set-Cookie: session=abc123; Path=/api; Domain=example.com; Max-Age=3600; "
        "HttpOnly; Secure; SameSite=Strict\r\n") != NULL);

    free_conn(conn);
}

static void test_multiple_cookies_each_get_own_line(void) {
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    res_status(&res, 200);
    res_set_cookie(&res, "a", "1", NULL);
    res_set_cookie(&res, "b", "2", NULL);
    /* Same name twice: unlike res_set_header, this does NOT overwrite -
     * both lines are sent. */
    res_set_cookie(&res, "a", "3", NULL);
    res_send(&res, "body");

    assert(conn->out_buf != NULL);
    assert(strstr(conn->out_buf, "Set-Cookie: a=1; Path=/\r\n") != NULL);
    assert(strstr(conn->out_buf, "Set-Cookie: b=2; Path=/\r\n") != NULL);
    assert(strstr(conn->out_buf, "Set-Cookie: a=3; Path=/\r\n") != NULL);

    free_conn(conn);
}

static void test_clear_cookie_expires_immediately(void) {
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    res_status(&res, 200);
    res_clear_cookie(&res, "session", NULL);
    res_send(&res, "body");

    assert(conn->out_buf != NULL);
    assert(strstr(conn->out_buf, "Set-Cookie: session=; Path=/; Max-Age=0\r\n") != NULL);

    free_conn(conn);
}

static void test_max_response_cookies_enforced(void) {
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    res_status(&res, 200);
    for (int i = 0; i < MAX_RESPONSE_COOKIES + 4; i++) {
        char name[32];
        snprintf(name, sizeof(name), "c%d", i);
        res_set_cookie(&res, name, "v", NULL);
    }

    /* Extras beyond the fixed cap are dropped, not overflowed into. */
    assert(res.set_cookie_count == MAX_RESPONSE_COOKIES);

    res_send(&res, "body");
    assert(conn->out_buf != NULL);

    free_conn(conn);
}

static void test_redirect_default_status(void) {
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    res_redirect(&res, 0, "/login");

    assert(conn->out_buf != NULL);
    assert(strstr(conn->out_buf, "HTTP/1.1 302 Found\r\n") != NULL);
    assert(strstr(conn->out_buf, "Location: /login\r\n") != NULL);
    assert(strstr(conn->out_buf, "Redirecting to /login") != NULL);

    free_conn(conn);
}

static void test_redirect_explicit_status(void) {
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    res_redirect(&res, 301, "/new-path");

    assert(conn->out_buf != NULL);
    assert(strstr(conn->out_buf, "HTTP/1.1 301 Moved Permanently\r\n") != NULL);
    assert(strstr(conn->out_buf, "Location: /new-path\r\n") != NULL);

    free_conn(conn);
}

static void test_chunked_streaming_basic(void) {
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    res_write(&res, "hello ", 6);
    res_write(&res, "world", 5);
    res_end(&res);

    assert(conn->out_buf != NULL);
    assert(strstr(conn->out_buf, "Transfer-Encoding: chunked\r\n") != NULL);
    assert(strstr(conn->out_buf, "Content-Length:") == NULL);
    assert(strstr(conn->out_buf, "\r\n\r\n6\r\nhello \r\n5\r\nworld\r\n0\r\n\r\n") != NULL);

    free_conn(conn);
}

static void test_chunked_streaming_trailers(void) {
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    res_set_trailer(&res, "Server-Timing", "app;dur=15.2");
    res_write(&res, "payload", 7);
    res_end(&res);

    assert(conn->out_buf != NULL);
    assert(strstr(conn->out_buf, "Trailer: Server-Timing\r\n") != NULL);
    assert(strstr(conn->out_buf, "7\r\npayload\r\n0\r\nServer-Timing: app;dur=15.2\r\n\r\n") != NULL);

    free_conn(conn);
}

static void test_chunked_streaming_forbidden_trailers(void) {
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    res_set_trailer(&res, "Transfer-Encoding", "chunked");
    res_set_trailer(&res, "Content-Length", "100");
    res_set_trailer(&res, "Trailer", "foo");

    assert(res.trailer_count == 0);

    free_conn(conn);
}

static void test_chunked_head_request_omits_body(void) {
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0, .is_head_request = 1 };

    res_write(&res, "suppressed", 10);
    res_end(&res);

    assert(conn->out_buf != NULL);
    assert(strstr(conn->out_buf, "Transfer-Encoding: chunked\r\n") != NULL);
    /* HEAD response terminates after headers double-CRLF; no chunk framing */
    assert(strstr(conn->out_buf, "suppressed") == NULL);
    assert(strstr(conn->out_buf, "0\r\n") == NULL);

    free_conn(conn);
}

static void test_send_file_non_existent(void) {
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    int rc = res_send_file(&res, "text/plain", "does_not_exist_xyz123.txt");
    assert(rc == -1);
    assert(conn->file_fd == -1);

    free_conn(conn);
}

static void test_send_file_headers_and_fd(void) {
    Connection *conn = make_conn();
    Response res = { .conn = conn, .status = 0 };

    int rc = res_send_file(&res, "text/plain", "README.md");
    assert(rc == 0);
    assert(conn->file_fd >= 0);
    assert(conn->file_remaining > 0);
    assert(conn->out_buf != NULL);
    assert(strstr(conn->out_buf, "Content-Type: text/plain\r\n") != NULL);
    assert(strstr(conn->out_buf, "Content-Length:") != NULL);

    close(conn->file_fd);
    conn->file_fd = -1;
    free_conn(conn);
}

static void test_res_init_needs_no_zeroed_struct(void) {
    /* res_init sets only scalars/counts; arrays are never read past their count. Start from garbage. */
    Connection *conn = make_conn();
    Response res;
    memset(&res, 0xA5, sizeof(res));
    res_init(&res, conn);
    assert(res.conn == conn && res.status == 200);
    assert(res.header_count == 0 && res.set_cookie_count == 0 && res.trailer_count == 0);
    assert(res.is_head_request == 0 && res.is_chunked == 0 && res.headers_sent == 0 && res.stream_ended == 0);

    res_send(&res, "hi");
    assert(conn->out_buf != NULL);
    size_t len;
    char *resp = without_date(conn->out_buf, conn->out_len, &len);
    assert(strcmp(resp, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\nConnection: keep-alive\r\n\r\nhi") == 0);
    assert(len == strlen(resp));
    free(resp);
    free_conn(conn);
}

static void test_exact_head_bytes(void) {
    /* Pins the wire format of the head builder: order, spacing, CRLFs. */
    Connection *conn = make_conn();
    Response res;
    res_init(&res, conn);
    res_status(&res, 201);
    res_set_header(&res, "X-A", "1");
    res_set_header(&res, "Content-Type", "application/json");
    res_set_cookie(&res, "s", "v", NULL);
    res_json(&res, "{}");
    const char *expected =
        "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nContent-Length: 2\r\nConnection: keep-alive\r\n"
        "X-A: 1\r\nSet-Cookie: s=v; Path=/\r\n\r\n{}";
    size_t len;
    char *resp = without_date(conn->out_buf, conn->out_len, &len);
    assert(len == strlen(expected));
    assert(memcmp(resp, expected, len) == 0);
    free(resp);
    free_conn(conn);

    /* Content-Length of a big body is formatted in full (no truncation in the integer writer). */
    conn = make_conn();
    res_init(&res, conn);
    char *big = malloc(1234568);
    memset(big, 'x', 1234567);
    big[1234567] = '\0';
    res_send(&res, big);
    const char *big_head = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 1234567\r\n";
    resp = without_date(conn->out_buf, conn->out_len, &len);
    assert(memcmp(resp, big_head, strlen(big_head)) == 0);
    free(resp);
    free(big);
    free_conn(conn);
}

static void test_second_send_replaces_first_without_leaking(void) {
    /* A handler that sends twice (e.g. an error path that falls through) gets "last wins";
     * the first buffer is freed, not leaked. */
    Connection *conn = make_conn();
    Response res;
    res_init(&res, conn);
    res_send(&res, "first");
    res_status(&res, 500);
    res_send(&res, "second");
    assert(conn->out_len == strlen("HTTP/1.1 500 Internal Server Error\r\nDate: Thu, 01 Jan 1970 00:00:00 GMT\r\n"
                                   "Content-Type: text/plain\r\nContent-Length: 6\r\nConnection: keep-alive\r\n\r\nsecond"));
    assert(memcmp(conn->out_buf + conn->out_len - 6, "second", 6) == 0);
    free_conn(conn);
}

static void test_oversized_head_drops_connection(void) {
    /* 16 headers of ~300 bytes cannot exceed the 8 KB head buffer; a cookie storm plus headers can.
     * Whatever does not fit must abort the response cleanly rather than truncate it. */
    Connection *conn = make_conn();
    Response res;
    res_init(&res, conn);
    char value[256];
    memset(value, 'v', sizeof(value) - 1);
    value[sizeof(value) - 1] = '\0';
    for (int i = 0; i < MAX_RESPONSE_HEADERS; i++) {
        char name[16];
        snprintf(name, sizeof(name), "X-H%d", i);
        res_set_header(&res, name, value);
    }
    static const CookieOptions opts = { .max_age = 0, .path = NULL, .domain = NULL,
                                        .http_only = 0, .secure = 0, .same_site = COOKIE_SAMESITE_UNSET };
    char cookie_value[400];
    memset(cookie_value, 'c', sizeof(cookie_value) - 1);
    cookie_value[sizeof(cookie_value) - 1] = '\0';
    for (int i = 0; i < MAX_RESPONSE_COOKIES; i++) {
        char name[16];
        snprintf(name, sizeof(name), "c%d", i);
        res_set_cookie(&res, name, cookie_value, &opts);
    }
    res_send(&res, "body");
    /* 16*~262 + 16*~410 = ~10.7 KB > 8 KB head buffer */
    assert(conn->out_len == 0);
    assert(conn->keep_alive == 0);
    free_conn(conn);
}

static void test_zero_cookie_options_is_a_session_cookie(void) {
    /* Regression: docs promised {0} meant "session cookie" but it emitted Max-Age=0 (expire now). */
    Connection *conn = make_conn();
    Response res;
    res_init(&res, conn);
    const CookieOptions zero = { 0 };
    res_set_cookie(&res, "s", "v", &zero);
    res_send(&res, "x");
    assert(strstr(conn->out_buf, "Set-Cookie: s=v; Path=/\r\n") != NULL);
    assert(strstr(conn->out_buf, "Max-Age") == NULL);
    free_conn(conn);

    conn = make_conn();
    res_init(&res, conn);
    const CookieOptions expire_now = { .max_age = -1 };
    res_set_cookie(&res, "s", "", &expire_now);
    res_send(&res, "x");
    assert(strstr(conn->out_buf, "Set-Cookie: s=; Path=/; Max-Age=0\r\n") != NULL);
    free_conn(conn);
}

static void test_header_injection_is_refused(void) {
    /* Regression: values were written verbatim, so decoded request data containing CR LF could inject
     * a second header or a whole second response (HTTP response splitting). */
    Connection *conn = make_conn();
    Response res;
    res_init(&res, conn);
    res_set_header(&res, "X-Ok", "fine\tvalue \xC3\xA9");
    res_set_header(&res, "X-Bad", "a\r\nSet-Cookie: session=attacker");
    res_set_header(&res, "X-Bad2", "a\nb");
    res_set_header(&res, "X-Bad3", "a\rb");
    res_set_header(&res, "X\r\nInjected", "v");
    res_set_header(&res, "Bad:Name", "v");
    res_set_header(&res, "", "v");
    assert(res.header_count == 1);
    res_send(&res, "x");
    assert(strstr(conn->out_buf, "X-Ok: fine\tvalue \xC3\xA9\r\n") != NULL);
    assert(strstr(conn->out_buf, "attacker") == NULL);
    assert(strstr(conn->out_buf, "Injected") == NULL);
    free_conn(conn);
}

static void test_redirect_refuses_injected_location(void) {
    Connection *conn = make_conn();
    Response res;
    res_init(&res, conn);
    res_redirect(&res, 302, "/home\r\nSet-Cookie: session=attacker\r\n\r\n<script>");
    assert(strncmp(conn->out_buf, "HTTP/1.1 500 Internal Server Error", 34) == 0);
    assert(strstr(conn->out_buf, "attacker") == NULL);
    assert(strstr(conn->out_buf, "Location") == NULL);
    free_conn(conn);

    conn = make_conn();
    res_init(&res, conn);
    res_redirect(&res, 301, "/a b/c?d=e&f=%20g");
    assert(strncmp(conn->out_buf, "HTTP/1.1 301 Moved Permanently", 30) == 0);
    assert(strstr(conn->out_buf, "Location: /a b/c?d=e&f=%20g\r\n") != NULL);
    free_conn(conn);
}

static void test_cookie_injection_is_refused(void) {
    Connection *conn = make_conn();
    Response res;
    res_init(&res, conn);
    res_set_cookie(&res, "ok", "v", NULL);
    res_set_cookie(&res, "a", "x\r\nSet-Cookie: b=1", NULL);   /* CR LF in value */
    res_set_cookie(&res, "a", "x; Domain=evil.example", NULL);  /* attribute injection via ';' */
    res_set_cookie(&res, "n;ame", "v", NULL);
    res_set_cookie(&res, "n=ame", "v", NULL);
    res_set_cookie(&res, "", "v", NULL);
    const CookieOptions bad_path = { .path = "/;Domain=evil.example" };
    res_set_cookie(&res, "p", "v", &bad_path);
    const CookieOptions bad_domain = { .domain = "evil.example\r\nX: y" };
    res_set_cookie(&res, "d", "v", &bad_domain);
    assert(res.set_cookie_count == 1);
    res_send(&res, "x");
    assert(strstr(conn->out_buf, "Set-Cookie: ok=v; Path=/\r\n") != NULL);
    assert(strstr(conn->out_buf, "evil") == NULL);
    free_conn(conn);
}

static void test_trailer_injection_is_refused(void) {
    Connection *conn = make_conn();
    Response res;
    res_init(&res, conn);
    res_set_trailer(&res, "Server-Timing", "db;dur=5");
    res_set_trailer(&res, "X-Bad", "a\r\nb");
    assert(res.trailer_count == 1);
    free_conn(conn);
}

/* every head carries a Date line, and the engine owns it (res_set_header refuses a custom one). */
static void test_date_header_present_and_reserved(void) {
    Connection *conn = make_conn();
    Response res;
    res_init(&res, conn);
    res_set_header(&res, "date", "Mon, 01 Jan 2001 00:00:00 GMT");
    assert(res.header_count == 0);
    res_send(&res, "x");
    size_t len;
    char *resp = without_date(conn->out_buf, conn->out_len, &len); /* asserts the Date line's shape */
    assert(strstr(resp, "Date") == NULL);                           /* exactly one Date line */
    free(resp);
    free_conn(conn);
}

/* 1xx/204/304 are bodiless - no Content-Length / Transfer-Encoding / default Content-Type, no body
 * bytes - on every sending path, so a keep-alive client never reads stray bytes as the next response. */
static void test_bodiless_statuses_send_head_only(void) {
    const int statuses[] = { 204, 304, 103 };
    for (size_t i = 0; i < sizeof(statuses) / sizeof(statuses[0]); i++) {
        Connection *conn = make_conn();
        Response res;
        res_init(&res, conn);
        res_status(&res, statuses[i]);
        res_set_header(&res, "ETag", "\"v1\"");
        res_send(&res, "must not be sent");
        size_t len;
        char *resp = without_date(conn->out_buf, conn->out_len, &len);
        char expected[160];
        snprintf(expected, sizeof(expected), "HTTP/1.1 %d %s\r\nConnection: keep-alive\r\nETag: \"v1\"\r\n\r\n",
                 statuses[i], status_text(statuses[i]));
        assert(strcmp(resp, expected) == 0);
        free(resp);
        free_conn(conn);
    }

    /* An explicit Content-Type is the handler's choice and is kept (RFC 9110 allows it on a 304). */
    Connection *conn = make_conn();
    Response res;
    res_init(&res, conn);
    res_status(&res, 304);
    res_set_header(&res, "Content-Type", "text/html");
    res_send(&res, "<p>");
    size_t len;
    char *resp = without_date(conn->out_buf, conn->out_len, &len);
    assert(strcmp(resp, "HTTP/1.1 304 Not Modified\r\nContent-Type: text/html\r\nConnection: keep-alive\r\n\r\n") == 0);
    free(resp);
    free_conn(conn);

    /* Chunked path: no Transfer-Encoding, no chunk bytes, no terminating chunk, no Trailer. */
    conn = make_conn();
    res_init(&res, conn);
    res_status(&res, 204);
    res_set_trailer(&res, "X-Sum", "1");
    res_write(&res, "abc", 3);
    res_end(&res);
    resp = without_date(conn->out_buf, conn->out_len, &len);
    assert(strcmp(resp, "HTTP/1.1 204 No Content\r\nConnection: keep-alive\r\n\r\n") == 0);
    free(resp);
    free_conn(conn);

    /* File path: head only, no fd left to stream. */
    conn = make_conn();
    res_init(&res, conn);
    res_status(&res, 204);
    char path[] = "/tmp/cexpress_c3_XXXXXX";
    const int tmp = mkstemp(path);
    assert(tmp >= 0);
    assert(write(tmp, "data", 4) == 4);
    close(tmp);
    assert(res_send_file(&res, "text/plain", path) == 0);
    assert(conn->file_fd == -1 && conn->file_remaining == 0);
    resp = without_date(conn->out_buf, conn->out_len, &len);
    assert(strcmp(resp, "HTTP/1.1 204 No Content\r\nConnection: keep-alive\r\n\r\n") == 0);
    free(resp);
    unlink(path);
    free_conn(conn);
}

int main(void) {
    test_date_header_present_and_reserved();
    test_bodiless_statuses_send_head_only();
    test_custom_header_is_sent();
    test_repeated_set_header_overwrites_case_insensitively();
    test_reserved_headers_are_rejected();
    test_custom_content_type_overrides_default();
    test_max_response_headers_enforced();
    test_head_response_omits_body_but_keeps_content_length();
    test_set_cookie_defaults();
    test_set_cookie_with_options();
    test_multiple_cookies_each_get_own_line();
    test_clear_cookie_expires_immediately();
    test_max_response_cookies_enforced();
    test_redirect_default_status();
    test_redirect_explicit_status();
    test_chunked_streaming_basic();
    test_chunked_streaming_trailers();
    test_chunked_streaming_forbidden_trailers();
    test_chunked_head_request_omits_body();
    test_send_file_non_existent();
    test_send_file_headers_and_fd();
    test_res_init_needs_no_zeroed_struct();
    test_exact_head_bytes();
    test_second_send_replaces_first_without_leaking();
    test_oversized_head_drops_connection();
    test_zero_cookie_options_is_a_session_cookie();
    test_header_injection_is_refused();
    test_redirect_refuses_injected_location();
    test_cookie_injection_is_refused();
    test_trailer_injection_is_refused();

    printf("all response tests passed\n");
    return 0;
}
