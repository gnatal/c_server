/*
 * Regression and hardening tests for lib/http_parser.c. Each test pins a bug that was found and fixed
 * (substring header matching, silent method truncation, lenient Content-Length, reliance on a zeroed
 * Request, ...) or a boundary of the hand-written scanners. Split from test_http_parser.c to keep both
 * files well under the project's 1,000-line cap.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_types.h"
#include "http_parser.h"
#include "arena.h"

Arena test_arena;
char test_arena_buf[64 * 1024];

static void test_framing_is_line_anchored(void) {
    /* Regression: Content-Length used to be found by substring search, so a header whose name merely
     * ends in it (X-Content-Length) - or "Content-Length:" inside the request target - made the server
     * wait for a body that never comes, and let a front proxy and this server disagree on framing. */
    const char *x_header = "GET /a HTTP/1.1\r\nHost: x\r\nX-Content-Length: 50\r\n\r\n";
    assert(extract_content_length(x_header) == 0);
    assert(request_is_complete(x_header, strlen(x_header)) == 1);

    const char *in_target = "GET /a?x=Content-Length:50 HTTP/1.1\r\nHost: x\r\n\r\n";
    assert(extract_content_length(in_target) == 0);
    assert(request_is_complete(in_target, strlen(in_target)) == 1);

    const char *in_value = "GET /a HTTP/1.1\r\nX-Note: Content-Length: 50\r\n\r\n";
    assert(request_is_complete(in_value, strlen(in_value)) == 1);

    /* Same for Transfer-Encoding: only the real header counts. */
    const char *te_lookalike = "POST /a HTTP/1.1\r\nX-Transfer-Encoding: chunked\r\nContent-Length: 0\r\n\r\n";
    assert(request_has_chunked_encoding(te_lookalike) == 0);

    /* A body that merely contains the text must not be scanned. */
    const char *in_body = "POST /a HTTP/1.1\r\nContent-Length: 20\r\n\r\nContent-Length: 999999";
    assert(extract_content_length(in_body) == 20);
    assert(request_is_complete(in_body, strlen(in_body)) == 1);

    Request req;
    assert(parse_http_request(x_header, strlen(x_header), &req, &test_arena) == 0);
    assert(req.content_length == 0);
    arena_reset(&test_arena);
}

static void test_content_length_is_strict(void) {
    assert(extract_content_length("Content-Length: 5abc\r\n") == -1);
    assert(extract_content_length("Content-Length: \r\n") == -1);
    assert(extract_content_length("Content-Length: 5, 5\r\n") == -1);
    assert(extract_content_length("Content-Length: +5\r\n") == -1);
    assert(extract_content_length("Content-Length:5\r\n") == 5);
    assert(extract_content_length("Content-Length:   7  \r\n") == 7);
    assert(extract_content_length("Content-Length:\t9\r\n") == 9);

    /* Duplicates: identical is fine, conflicting is rejected (smuggling shape). */
    assert(extract_content_length("Content-Length: 4\r\nContent-Length: 4\r\n") == 4);
    assert(extract_content_length("Content-Length: 4\r\nContent-Length: 9\r\n") == -1);
    assert(extract_content_length("Content-Length: 4\r\nContent-Length: 999999999999\r\n") == -1);

    /* Absurdly long digit runs neither overflow nor wrap to a small number. */
    assert(extract_content_length("Content-Length: 99999999999999999999999999\r\n") == -2);

    /* Content-Length together with chunked framing is rejected everywhere. */
    const char *both = "POST /a HTTP/1.1\r\nContent-Length: 4\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nWiki\r\n0\r\n\r\n";
    size_t header_len;
    int chunked;
    assert(request_framing(both, strlen(both), &header_len, &chunked, NULL, NULL) == -1);
    assert(chunked == 1);
    assert(request_is_complete(both, strlen(both)) == 1);
    Request req;
    assert(parse_http_request(both, strlen(both), &req, &test_arena) == -1);
    assert(req.body == NULL);
}

static void test_request_framing(void) {
    size_t header_len = 99;
    int chunked = 99;

    const char *partial = "POST /a HTTP/1.1\r\nContent-Length: 3\r\n";
    assert(request_framing(partial, strlen(partial), &header_len, &chunked, NULL, NULL) == 0);
    assert(header_len == 0 && chunked == 0);

    const char *complete = "POST /a HTTP/1.1\r\nContent-Length: 3\r\n\r\nabc";
    assert(request_framing(complete, strlen(complete), &header_len, &chunked, NULL, NULL) == 3);
    assert(header_len == strlen(complete) - 3 && chunked == 0);

    const char *no_headers = "GET / HTTP/1.1\r\n\r\n";
    assert(request_framing(no_headers, strlen(no_headers), &header_len, &chunked, NULL, NULL) == 0);
    assert(header_len == strlen(no_headers));

    const char *te = "POST /a HTTP/1.1\r\nTransfer-Encoding: gzip, chunked\r\n\r\n";
    assert(request_framing(te, strlen(te), &header_len, &chunked, NULL, NULL) == 0);
    assert(chunked == 1 && header_len == strlen(te));
}

/* request_framing's optional path_out/path_len_out (S4: app_use_body_limit needs the request path
 * before a Request struct exists). NULL for either stays valid (every other caller above passes
 * NULL, NULL); when given, they point into the caller's own buffer, not a copy. */
static void test_request_framing_path_out(void) {
    size_t header_len;
    int chunked;
    const char *path;
    size_t path_len;

    const char *req_line = "POST /api/uploads/avatar HTTP/1.1\r\nContent-Length: 3\r\n\r\nabc";
    assert(request_framing(req_line, strlen(req_line), &header_len, &chunked, &path, &path_len) == 3);
    assert(path_len == strlen("/api/uploads/avatar"));
    assert(memcmp(path, "/api/uploads/avatar", path_len) == 0);

    /* Headers incomplete: header_len == 0, path_out must not be trusted (not asserted here, just
     * confirmed request_framing doesn't crash when path_out is requested but unavailable). */
    const char *partial = "POST /a HTTP/1.1\r\nContent-Length: 3\r\n";
    assert(request_framing(partial, strlen(partial), &header_len, &chunked, &path, &path_len) == 0);
    assert(header_len == 0);
}

static void test_wants_close_header_forms(void) {
    Request req;
    memset(&req, 0, sizeof(req));
    strncpy(req.version, "HTTP/1.1", sizeof(req.version) - 1);

    /* Regression: "Connection:close" (no space) used to be missed by a substring search. */
    parse_headers("Connection:close\r\n", &req);
    assert(request_wants_close(&req) == 1);
    parse_headers("connection: CLOSE\r\n", &req);
    assert(request_wants_close(&req) == 1);
    parse_headers("Connection: keep-alive, Upgrade\r\n", &req);
    assert(request_wants_close(&req) == 0);
    parse_headers("Connection: upgrade, close\r\n", &req);
    assert(request_wants_close(&req) == 1);
    /* Only a real Connection header counts, not one quoted inside another header's value. */
    parse_headers("X-Debug: Connection: close\r\n", &req);
    assert(request_wants_close(&req) == 0);
    /* Token match, not substring: "closed" is not "close". */
    parse_headers("Connection: closed-form\r\n", &req);
    assert(request_wants_close(&req) == 0);
}

static void test_request_line_limits(void) {
    Request req;
    /* Regression: a method token longer than req->method used to be silently truncated to its first
     * 7 characters, with the leftover characters then mis-read as the path. */
    const char *long_method = "GETTTTTTTTTT /a HTTP/1.1\r\n\r\n";
    assert(parse_http_request(long_method, strlen(long_method), &req, &test_arena) == -1);
    assert(req.body == NULL);

    const char *seven = "OPTIONS /a HTTP/1.1\r\n\r\n";
    assert(parse_http_request(seven, strlen(seven), &req, &test_arena) == 0);
    assert(strcmp(req.method, "OPTIONS") == 0);
    arena_reset(&test_arena);

    const char *eight = "OPTIONSS /a HTTP/1.1\r\n\r\n";
    assert(parse_http_request(eight, strlen(eight), &req, &test_arena) == -1);

    const char *no_target = "GET\r\n\r\n";
    assert(parse_http_request(no_target, strlen(no_target), &req, &test_arena) == -1);
    const char *leading_space = " GET /a HTTP/1.1\r\n\r\n";
    assert(parse_http_request(leading_space, strlen(leading_space), &req, &test_arena) == -1);

    /* Version is required by picohttpparser. */
    const char *no_version = "GET /a\r\n\r\n";
    assert(parse_http_request(no_version, strlen(no_version), &req, &test_arena) == -1);
    arena_reset(&test_arena);

    /* An oversized query is truncated to fit req->query, never overflowed. */
    char long_query[1200];
    int n = snprintf(long_query, sizeof(long_query), "GET /a?");
    for (int i = 0; i < 900; i++) long_query[n++] = 'q';
    n += snprintf(long_query + n, sizeof(long_query) - (size_t)n, " HTTP/1.1\r\n\r\n");
    assert(parse_http_request(long_query, (size_t)n, &req, &test_arena) == 0);
    assert(strlen(req.query) == sizeof(req.query) - 1);
    assert(req.query_count == 1);
    arena_reset(&test_arena);
}

/* parse_http_request initializes only what accessors read. Parse into a struct full of garbage
 * and check nothing observable depends on the old memset(0). */
static void test_parse_does_not_depend_on_zeroed_request(void) {
    Request req;
    memset(&req, 0xA5, sizeof(req));
    const char *raw = "GET /plain HTTP/1.1\r\n\r\n";
    assert(parse_http_request(raw, strlen(raw), &req, &test_arena) == 0);
    assert(strcmp(req.method, "GET") == 0);
    assert(strcmp(req.path, "/plain") == 0);
    assert(strcmp(req.query, "") == 0);
    assert(req.query_count == 0 && req.header_count == 0 && req.cookie_count == 0 && req.param_count == 0);
    assert(req.content_length == 0);
    assert(req.body != NULL && req.body[0] == '\0');
    assert(req_get_query(&req, "q") == NULL);
    assert(req_get_header(&req, "Host") == NULL);
    assert(req_get_cookie(&req, "a") == NULL);
    arena_reset(&test_arena);

    memset(&req, 0xA5, sizeof(req));
    const char *bad = "GET\r\n\r\n";
    assert(parse_http_request(bad, strlen(bad), &req, &test_arena) == -1);
    assert(req.body == NULL);
    assert(req.content_length == 0);
}

static void test_header_value_whitespace_and_limits(void) {
    Request req;
    memset(&req, 0, sizeof(req));

    /* Optional whitespace (spaces and tabs) around the value is trimmed on both sides. */
    parse_headers("A:   spaced   \r\nB:\ttabbed\t\r\nC:x y  z\r\n", &req);
    assert(strcmp(req_get_header(&req, "A"), "spaced") == 0);
    assert(strcmp(req_get_header(&req, "B"), "tabbed") == 0);
    assert(strcmp(req_get_header(&req, "C"), "x y  z") == 0);

    /* parse_headers (unlike parse_http_request, S5) has no way to signal an error - it's a `void`
     * component parser exposed for tests, not called from the live request path (that path duplicates
     * this loop inline in parse_http_request so it can return -3 instead). It still truncates
     * silently to whatever the array holds, so an over-long name or value is still truncated to its
     * slot, still NUL-terminated - the value here (1100 chars) exceeds the raised MAX_HEADER_VALUE_LEN
     * (1024) that a Bearer JWT or long cookie would now fit under; see
     * test_parse_http_request_rejects_oversized_header_431 below for the production 431 behavior. */
    char big[2048];
    int n = 0;
    for (int i = 0; i < 100; i++) big[n++] = 'N';
    n += snprintf(big + n, sizeof(big) - (size_t)n, ": ");
    for (int i = 0; i < 1100; i++) big[n++] = 'v';
    big[n++] = '\r';
    big[n++] = '\n';
    big[n] = '\0';
    parse_headers(big, &req);
    assert(req.header_count == 1);
    assert(strlen(req.header_names[0]) == sizeof(req.header_names[0]) - 1);
    assert(strlen(req.header_values[0]) == sizeof(req.header_values[0]) - 1);
}

/* S5: parse_http_request (the function actually on the live request path, unlike parse_headers above)
 * rejects an over-long header name or value with -3 instead of silently truncating it - the fix for
 * the exact problem improvements.md measured: a 400-character Bearer token used to be stored as 255
 * characters with a successful (wrong) parse. */
static void test_parse_http_request_rejects_oversized_header_431(void) {
    Request req;

    /* A value just past the new cap (1024) is rejected: a Bearer token in this range used to be the
     * silently-truncated case improvements.md measured (at the old, smaller 255-byte cap). */
    char oversized_value[1400];
    int n = snprintf(oversized_value, sizeof(oversized_value), "POST /a HTTP/1.1\r\nAuthorization: Bearer ");
    for (int i = 0; i < 1100; i++) oversized_value[n++] = 'x';
    n += snprintf(oversized_value + n, sizeof(oversized_value) - (size_t)n, "\r\nContent-Length: 0\r\n\r\n");
    assert(parse_http_request(oversized_value, (size_t)n, &req, &test_arena) == -3);

    /* A value comfortably within the new cap (a realistic ~600-char JWT-shaped token) parses fine and
     * is NOT truncated - this is the actual compatibility half of the fix, not just the loud-failure
     * half: JWTs in the 300-1,000 char range improvements.md called out now round-trip exactly. */
    char within_cap[1200];
    n = snprintf(within_cap, sizeof(within_cap), "POST /a HTTP/1.1\r\nAuthorization: Bearer ");
    const int token_len = 600;
    for (int i = 0; i < token_len; i++) within_cap[n++] = 'a' + (i % 26);
    n += snprintf(within_cap + n, sizeof(within_cap) - (size_t)n, "\r\nContent-Length: 0\r\n\r\n");
    assert(parse_http_request(within_cap, (size_t)n, &req, &test_arena) == 0);
    const char *auth = req_get_header(&req, "Authorization");
    assert(auth != NULL);
    assert(strlen(auth) == strlen("Bearer ") + (size_t)token_len); /* exact, not truncated */

    /* An over-long header NAME is rejected the same way, for the same reason (never silently cut a
     * header to fit, whichever side of the colon overflows). */
    char oversized_name[400];
    n = snprintf(oversized_name, sizeof(oversized_name), "POST /a HTTP/1.1\r\n");
    for (int i = 0; i < 100; i++) oversized_name[n++] = 'N';
    n += snprintf(oversized_name + n, sizeof(oversized_name) - (size_t)n, ": v\r\nContent-Length: 0\r\n\r\n");
    assert(parse_http_request(oversized_name, (size_t)n, &req, &test_arena) == -3);
}

static void test_percent_decoding_at_boundaries(void) {
    Request req;
    memset(&req, 0, sizeof(req));

    parse_query_string("a=%41&b=%4&c=%&d=%zz&e=x%", &req);
    assert(strcmp(req_get_query(&req, "a"), "A") == 0);
    assert(strcmp(req_get_query(&req, "b"), "%4") == 0);   /* truncated escape stays literal */
    assert(strcmp(req_get_query(&req, "c"), "%") == 0);
    assert(strcmp(req_get_query(&req, "d"), "%zz") == 0);
    assert(strcmp(req_get_query(&req, "e"), "x%") == 0);

    /* An empty name with a value is kept; a lone "=" gives empty name and value. */
    parse_query_string("=v&=", &req);
    assert(req.query_count == 2);
    assert(strcmp(req.query_names[0], "") == 0 && strcmp(req.query_values[0], "v") == 0);

    /* An over-long value is truncated to its slot; decoding never reads or writes past it. */
    char long_value[400] = "k=";
    memset(long_value + 2, '%', 3);
    memset(long_value + 5, '4', 1);
    memset(long_value + 6, '1', 300);
    long_value[306] = '\0';
    parse_query_string(long_value, &req);
    assert(req.query_count == 1);
    assert(strlen(req.query_values[0]) <= sizeof(req.query_values[0]) - 1);
}

int main(void) {
    arena_init(&test_arena, test_arena_buf, sizeof(test_arena_buf));
    test_framing_is_line_anchored();
    test_content_length_is_strict();
    test_request_framing();
    test_request_framing_path_out();
    test_parse_http_request_rejects_oversized_header_431();
    test_wants_close_header_forms();
    test_request_line_limits();
    test_parse_does_not_depend_on_zeroed_request();
    test_header_value_whitespace_and_limits();
    test_percent_decoding_at_boundaries();
    printf("all http hardening tests passed\n");
    return 0;
}
