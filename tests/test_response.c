#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_types.h"
#include "response.h"

static Connection *make_conn(void) {
    Connection *conn = calloc(1, sizeof(Connection));
    conn->keep_alive = 1;
    return conn;
}

static void free_conn(Connection *conn) {
    free(conn->out_buf);
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

int main(void) {
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

    printf("all response tests passed\n");
    return 0;
}
