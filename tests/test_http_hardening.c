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

    /* "gzip, chunked" is no longer accepted as chunked framing - this engine implements exactly one
     * transfer-coding ("chunked", named alone), and layering an unimplemented coding like gzip on top
     * used to be silently accepted via a bare substring search for "chunked" in the header value. Now
     * rejected (-3, -> 501) - see test_transfer_encoding_token_matching for the full matrix. */
    const char *te = "POST /a HTTP/1.1\r\nTransfer-Encoding: gzip, chunked\r\n\r\n";
    assert(request_framing(te, strlen(te), &header_len, &chunked, NULL, NULL) == -3);
    assert(chunked == 0 && header_len == strlen(te));
}

/* request_framing's optional path_out/path_len_out (app_use_body_limit needs the request path
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
    parse_headers("Connection:close\r\n", &req, &test_arena);
    assert(request_wants_close(&req) == 1);
    parse_headers("connection: CLOSE\r\n", &req, &test_arena);
    assert(request_wants_close(&req) == 1);
    parse_headers("Connection: keep-alive, Upgrade\r\n", &req, &test_arena);
    assert(request_wants_close(&req) == 0);
    parse_headers("Connection: upgrade, close\r\n", &req, &test_arena);
    assert(request_wants_close(&req) == 1);
    /* Only a real Connection header counts, not one quoted inside another header's value. */
    parse_headers("X-Debug: Connection: close\r\n", &req, &test_arena);
    assert(request_wants_close(&req) == 0);
    /* Token match, not substring: "closed" is not "close". */
    parse_headers("Connection: closed-form\r\n", &req, &test_arena);
    assert(request_wants_close(&req) == 0);
    arena_reset(&test_arena);
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
    parse_headers("A:   spaced   \r\nB:\ttabbed\t\r\nC:x y  z\r\n", &req, &test_arena);
    assert(strcmp(req_get_header(&req, "A"), "spaced") == 0);
    assert(strcmp(req_get_header(&req, "B"), "tabbed") == 0);
    assert(strcmp(req_get_header(&req, "C"), "x y  z") == 0);
    arena_reset(&test_arena);

    /* parse_headers stores views into header_block now, the same as the live request path
     * (parse_http_request_from_head) - there is no fixed-size slot left to truncate into, so a header
     * name or value far past the old 255/1024-byte caps round-trips through req_get_header exactly,
     * uncut. header_block must outlive the req_get_header call below (it does: it's a local array). */
    char big[2048];
    int n = 0;
    for (int i = 0; i < 100; i++) big[n++] = 'N';
    n += snprintf(big + n, sizeof(big) - (size_t)n, ": ");
    for (int i = 0; i < 1100; i++) big[n++] = 'v';
    big[n++] = '\r';
    big[n++] = '\n';
    big[n] = '\0';
    parse_headers(big, &req, &test_arena);
    assert(req.header_count == 1);
    /* req.headers[0].name is a view, not NUL-terminated - materialize it the same way req_get_header
     * would, just to hand its exact 100-char name to req_get_header as a C string. */
    char name_buf[128];
    memcpy(name_buf, req.headers[0].name, req.headers[0].name_len);
    name_buf[req.headers[0].name_len] = '\0';
    assert(strlen(name_buf) == 100);
    const char *value = req_get_header(&req, name_buf);
    assert(value != NULL && strlen(value) == 1100);
    arena_reset(&test_arena);
}

/* parse_http_request stores header views into `raw`, not fixed-size copies, so there is no per-
 * header length left to overflow - the "proper fix" improvements.md predicted (solved properly only by
 * storing views). A header name or value of any length that fits within a complete request round-
 * trips through req_get_header exactly; parse_http_request no longer returns -3 for this (that return
 * code is retired, see http_parser.h). */
static void test_parse_http_request_header_views_are_not_capped(void) {
    Request req;

    /* A value far past the old 1024-byte cap (a Bearer token in this range used to be the silently-
     * truncated case improvements.md measured, at the original, smaller 255-byte cap) now parses fine
     * and round-trips exactly. */
    char oversized_value[1400];
    int n = snprintf(oversized_value, sizeof(oversized_value), "POST /a HTTP/1.1\r\nAuthorization: Bearer ");
    const int oversized_len = 1100;
    for (int i = 0; i < oversized_len; i++) oversized_value[n++] = 'x';
    n += snprintf(oversized_value + n, sizeof(oversized_value) - (size_t)n, "\r\nContent-Length: 0\r\n\r\n");
    assert(parse_http_request(oversized_value, (size_t)n, &req, &test_arena) == 0);
    const char *auth1 = req_get_header(&req, "Authorization");
    assert(auth1 != NULL && strlen(auth1) == strlen("Bearer ") + (size_t)oversized_len);
    arena_reset(&test_arena);

    /* A realistic ~600-char JWT-shaped token, comfortably within even the old cap, is unaffected. */
    char within_cap[1200];
    n = snprintf(within_cap, sizeof(within_cap), "POST /a HTTP/1.1\r\nAuthorization: Bearer ");
    const int token_len = 600;
    for (int i = 0; i < token_len; i++) within_cap[n++] = 'a' + (i % 26);
    n += snprintf(within_cap + n, sizeof(within_cap) - (size_t)n, "\r\nContent-Length: 0\r\n\r\n");
    assert(parse_http_request(within_cap, (size_t)n, &req, &test_arena) == 0);
    const char *auth2 = req_get_header(&req, "Authorization");
    assert(auth2 != NULL);
    assert(strlen(auth2) == strlen("Bearer ") + (size_t)token_len); /* exact, not truncated */
    arena_reset(&test_arena);

    /* An over-long header NAME is unaffected the same way, for the same reason. */
    char oversized_name[400];
    n = snprintf(oversized_name, sizeof(oversized_name), "POST /a HTTP/1.1\r\n");
    for (int i = 0; i < 100; i++) oversized_name[n++] = 'N';
    n += snprintf(oversized_name + n, sizeof(oversized_name) - (size_t)n, ": v\r\nContent-Length: 0\r\n\r\n");
    assert(parse_http_request(oversized_name, (size_t)n, &req, &test_arena) == 0);
    char name_buf[128];
    memset(name_buf, 'N', 100);
    name_buf[100] = '\0';
    const char *v = req_get_header(&req, name_buf);
    assert(v != NULL && strcmp(v, "v") == 0);
    arena_reset(&test_arena);
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

static void test_embedded_nul_rejected(void) {
    /* url_decode itself: "%00" decodes to a real NUL - returns -1, but still writes and NUL-terminates
     * dst (the caller decides what to do with a rejected decode, same contract as decode_bounded). */
    char dst[64];
    assert(url_decode("style.css%00.png", dst, sizeof(dst), 0) == -1);
    assert(strcmp(dst, "style.css") == 0); /* what a strlen/strcmp-based check downstream would see */

    /* A path with no embedded NUL is unaffected. */
    assert(url_decode("style.css", dst, sizeof(dst), 0) == 0);
    assert(strcmp(dst, "style.css") == 0);

    /* parse_query_string: a %00 in either the name or the value of a pair is rejected (-1), not just
     * silently decoded - improvements.md names query names/values alongside the path. */
    Request req;
    memset(&req, 0, sizeof(req));
    assert(parse_query_string("a=1&b=x%00y", &req) == -1);
    memset(&req, 0, sizeof(req));
    assert(parse_query_string("x%00y=1", &req) == -1);
    /* An ordinary query string is unaffected. */
    memset(&req, 0, sizeof(req));
    assert(parse_query_string("a=1&b=2", &req) == 0);

    /* parse_http_request: the exact scenario improvements.md measured - "%00" in the request-target
     * path used to truncate it, so "/static/style.css%00.png" was routed as "/static/style.css" with a
     * 200. Now the whole request is rejected (-4) before routing ever sees it. */
    const char *nul_path = "GET /static/style.css%00.png HTTP/1.1\r\nHost: x\r\n\r\n";
    assert(parse_http_request(nul_path, strlen(nul_path), &req, &test_arena) == -4);

    /* A %00 in the query string alone (path itself clean) is rejected the same way. */
    const char *nul_query = "GET /search?q=a%00b HTTP/1.1\r\nHost: x\r\n\r\n";
    assert(parse_http_request(nul_query, strlen(nul_query), &req, &test_arena) == -4);

    /* An ordinary percent-encoded path/query (no embedded NUL) still parses normally. */
    const char *clean = "GET /static/style%2Ecss?q=a%20b HTTP/1.1\r\nHost: x\r\n\r\n";
    assert(parse_http_request(clean, strlen(clean), &req, &test_arena) == 0);
    assert(strcmp(req.path, "/static/style.css") == 0);
    arena_reset(&test_arena);
}

/* a request line picohttpparser rejects outright (no HTTP version, an unsupported version, plain
 * garbage) used to leave header_len at 0 - the same value a genuinely incomplete request line leaves it
 * at - so request_is_complete reported "need more bytes" forever instead of surfacing the malformed
 * request. request_head_is_complete now tells the two apart via content_length (set to -1 only on the
 * malformed path), so all three shapes below are reported complete and parse_http_request rejects them
 * with -1, the same code any other malformed request already got. */
static void test_malformed_request_line_rejected(void) {
    const char *no_version = "GET /\r\n\r\n";
    const char *bad_version = "GET / HTTP/2.0\r\n\r\n";
    const char *garbage = "this is not a request\r\n\r\n";
    const char *malformed[] = {no_version, bad_version, garbage};

    for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
        const char *line = malformed[i];
        size_t header_len = 99;
        int chunked = 99;
        assert(request_framing(line, strlen(line), &header_len, &chunked, NULL, NULL) == -1);
        assert(header_len == 0);

        /* The fix: request_is_complete now says "complete" (stop waiting) instead of "need more". */
        assert(request_is_complete(line, strlen(line)) == 1);

        Request req;
        assert(parse_http_request(line, strlen(line), &req, &test_arena) == -1);
        arena_reset(&test_arena);
    }

    /* A genuinely incomplete request line (no terminator yet) must still report "need more bytes" -
     * the fix must not widen "malformed" to cover this. */
    const char *incomplete = "GET / HTTP/1.1\r\n";
    assert(request_is_complete(incomplete, strlen(incomplete)) == 0);
    size_t header_len;
    int chunked;
    assert(request_framing(incomplete, strlen(incomplete), &header_len, &chunked, NULL, NULL) == 0);
    assert(header_len == 0);
}

/* picohttpparser accepts a bare '\n' as a line terminator anywhere one is expected - a leniency a
 * strict front proxy would not extend, letting the two disagree about request boundaries (smuggling
 * ambiguity). request_framing now rejects any request whose request line or header block contains a bare
 * '\n' (malformed, same shape as a malformed request line's -1: header_len == 0, request_is_complete reports "complete" so the
 * rejection surfaces immediately instead of waiting for more bytes that would just repeat the problem). */
static void test_bare_lf_rejected(void) {
    const char *bare_lf_lines[] = {
        "GET / HTTP/1.1\n\r\n",                       /* request line terminated by a lone LF */
        "GET / HTTP/1.1\r\nHost: x\n\r\n",             /* a header line terminated by a lone LF */
        "GET / HTTP/1.1\r\nHost: x\r\n\n",             /* the final blank line is a lone LF */
    };
    for (size_t i = 0; i < sizeof(bare_lf_lines) / sizeof(bare_lf_lines[0]); i++) {
        const char *line = bare_lf_lines[i];
        size_t header_len = 99;
        int chunked = 99;
        assert(request_framing(line, strlen(line), &header_len, &chunked, NULL, NULL) == -1);
        assert(header_len == 0);
        assert(request_is_complete(line, strlen(line)) == 1);

        Request req;
        assert(parse_http_request(line, strlen(line), &req, &test_arena) == -1);
        arena_reset(&test_arena);
    }

    /* An ordinary, fully CRLF-terminated request is unaffected. */
    const char *clean = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    assert(request_is_complete(clean, strlen(clean)) == 1);
    Request req;
    assert(parse_http_request(clean, strlen(clean), &req, &test_arena) == 0);
    arena_reset(&test_arena);
}

/* Transfer-Encoding is matched token-exact, not by substring. Only a value that reduces (after
 * splitting on commas and trimming OWS) to exactly one token, "chunked" (case-insensitive), is accepted -
 * this engine implements no other transfer-coding, so anything else (a substring lookalike, "chunked"
 * accompanied by any other coding regardless of order, or an unsupported coding alone) is rejected with
 * -3 (-> 501 at the connection layer) instead of either being silently ignored or mistaken for plain
 * chunked framing. */
static void test_transfer_encoding_token_matching(void) {
    struct {
        const char *header_line;
        int expect_chunked;
        int expect_code; /* content_length code: 0 (absent-equivalent), or -3 (unsupported/ambiguous) */
    } cases[] = {
        {"Transfer-Encoding: chunked\r\n", 1, 0},
        {"Transfer-Encoding: CHUNKED\r\n", 1, 0},
        {"Transfer-Encoding:   chunked  \r\n", 1, 0},       /* OWS around the token */
        {"Transfer-Encoding: xchunked\r\n", 0, -3},          /* substring, not a token - rejected */
        {"Transfer-Encoding: chunkedx\r\n", 0, -3},
        {"Transfer-Encoding: gzip\r\n", 0, -3},              /* unsupported coding, alone */
        {"Transfer-Encoding: gzip, chunked\r\n", 0, -3},     /* chunked present, but not alone */
        {"Transfer-Encoding: chunked, gzip\r\n", 0, -3},     /* chunked not last either way - rejected */
        {"Transfer-Encoding: chunked, chunked\r\n", 0, -3},  /* duplicate token: still not "exactly one" */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char req_buf[256];
        snprintf(req_buf, sizeof(req_buf), "POST /a HTTP/1.1\r\n%sContent-Length: 0\r\n\r\n",
                 cases[i].expect_code == 0 ? "" : cases[i].header_line);
        /* Cases expected to be accepted as chunked must not also carry Content-Length (smuggling
         * shape: chunked + Content-Length together is its own -1, unrelated to what's being tested
         * here), so build those without one. */
        if (cases[i].expect_code == 0) {
            snprintf(req_buf, sizeof(req_buf), "POST /a HTTP/1.1\r\n%s\r\n4\r\nWiki\r\n0\r\n\r\n",
                     cases[i].header_line);
        }
        size_t header_len = 99;
        int chunked = 99;
        const int code = request_framing(req_buf, strlen(req_buf), &header_len, &chunked, NULL, NULL);
        assert(chunked == cases[i].expect_chunked);
        /* request_framing's return is the Content-Length code, not chunked decode state - these cases
         * carry no Content-Length header, so accepted ones return 0 (absent), same as any other request
         * with no Content-Length header at all. */
        assert(code == cases[i].expect_code);
    }

    /* Splitting the coding list across two separate Transfer-Encoding header instances doesn't evade
     * the check: neither header line alone is the sole token "chunked". */
    const char *split = "POST /a HTTP/1.1\r\nTransfer-Encoding: gzip\r\nTransfer-Encoding: chunked\r\n\r\n";
    size_t header_len;
    int chunked;
    assert(request_framing(split, strlen(split), &header_len, &chunked, NULL, NULL) == -3);

    /* End to end: parse_http_request surfaces -3 as -1 (malformed) with req.content_length == -3, the
     * same generic "content_length < 0" plumbing the body-limit and malformed-line checks already use for their own sentinels. */
    const char *gzip_only = "POST /a HTTP/1.1\r\nTransfer-Encoding: gzip\r\nContent-Length: 0\r\n\r\n";
    Request req;
    assert(parse_http_request(gzip_only, strlen(gzip_only), &req, &test_arena) == -1);
    assert(req.content_length == -3);
    arena_reset(&test_arena);
}

/* picohttpparser's parse_http_version asks for 9 bytes before it looks at any, so a version token
 * shorter than "HTTP/1.x" plus a line end made a finished head read as "incomplete" - forever, since the
 * client has nothing left to send - and the request went unanswered until the request header deadline (408). Found by
 * fuzz_parser.c's answered-or-closed oracle. A head is malformed once a blank line has arrived without it parsing. */
static void test_short_version_token_rejected(void) {
    const char *malformed[] = {
        "GET / X\r\n\r\n",
        "GET / HTTP\r\n\r\n",
        "GET / HTTP/1\r\n\r\n",
        "GET  H&TTP/1. 1\r\n\r\n",            /* the fuzzer's own find */
        "GET / X\n\n",                          /* bare-LF blank line: also a finished head */
        "GET / X\r\nHost: y\r\n\r\n",           /* short token, then a header */
    };
    for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
        const char *line = malformed[i];
        size_t header_len = 99;
        int chunked = 99;
        assert(request_framing(line, strlen(line), &header_len, &chunked, NULL, NULL) == -1);
        assert(header_len == 0);
        assert(request_is_complete(line, strlen(line)) == 1);
        Request req;
        assert(parse_http_request(line, strlen(line), &req, &test_arena) == -1);
        arena_reset(&test_arena);
    }

    /* No blank line yet: still "need more bytes", however short the version token so far. */
    const char *incomplete[] = {
        "GET / X\r\n", "GET / X\r\n\r", "GET / HTTP/1.1\r\nHost: x\r\n", "GET / HTTP/1.1\r\nHost: x\r\n\r", "\r\n",
        "\r\nGET / HTTP/1.1\r\n",
    };
    for (size_t i = 0; i < sizeof(incomplete) / sizeof(incomplete[0]); i++) {
        assert(request_is_complete(incomplete[i], strlen(incomplete[i])) == 0);
    }

    /* The one leading empty line picohttpparser skips is still not mistaken for a finished head. */
    const char *leading_crlf = "\r\nGET /a HTTP/1.1\r\nHost: x\r\n\r\n";
    Request req;
    assert(request_is_complete(leading_crlf, strlen(leading_crlf)) == 1);
    assert(parse_http_request(leading_crlf, strlen(leading_crlf), &req, &test_arena) == 0);
    assert(strcmp(req.path, "/a") == 0);
    arena_reset(&test_arena);
}

/* 1 when buf[0..len) holds a blank line ('\n' then "\n" or "\r\n"): the end of any head. */
static int has_blank_line(const char *buf, const size_t len) {
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] == '\n' && (buf[i + 1] == '\n' || (buf[i + 1] == '\r' && i + 2 < len && buf[i + 2] == '\n'))) {
            return 1;
        }
    }
    return 0;
}

/* The status connection.c answers with (0 = dispatched), from a from-scratch parse of buf[0..len). */
static int answer_for(const char *buf, const size_t len) {
    char *exact = malloc(len ? len : 1);
    assert(exact != NULL);
    memcpy(exact, buf, len);
    Request req;
    const int st = parse_http_request(exact, len, &req, &test_arena);
    assert(st == 0 || st == -1 || st == -2 || st == -4); /* every status has an answer */
    const int answer = st == 0 ? 0 : st == -2 ? 414 : req.content_length == -2 ? 413 : req.content_length == -3 ? 501 : 400;
    arena_reset(&test_arena);
    free(exact);
    return answer;
}

/* A chunk-size line is 1*HEXDIG then optionally BWS ";" chunk-ext (RFC 9112 7.1), nothing else. strtoul
 * used to accept "0x5", "+5" and " 5": a front-end that reads "0x5" as 0 thinks the body ended while this
 * engine reads 5 more bytes - request smuggling. Trailer lines get the header block's line rules. */
static void test_chunk_size_line_is_strict(void) {
    const char *rejected[] = {
        "0x5\r\nhello\r\n0\r\n\r\n",
        "+5\r\nhello\r\n0\r\n\r\n",
        " 5\r\nhello\r\n0\r\n\r\n",
        "-5\r\nhello\r\n0\r\n\r\n",
        "\t5\r\nhello\r\n0\r\n\r\n",
        "5 \r\nhello\r\n0\r\n\r\n",                   /* BWS only before ";" */
        "5 5\r\nhello\r\n0\r\n\r\n",
        "5_\r\nhello\r\n0\r\n\r\n",
        "5;a\x01\r\nhello\r\n0\r\n\r\n",               /* control character in the extension */
        "5;a\rb\r\nhello\r\n0\r\n\r\n",                /* bare CR in the extension */
        ";e\r\n0\r\n\r\n",                              /* no digits */
        "0x0\r\n\r\n",
        " 0\r\n\r\n",
        "00000000000000005\r\nhello\r\n0\r\n\r\n",     /* 17 digits: more than size_t holds, even as zeros */
        "0\r\nX: a\nY: b\r\n\r\n",                       /* bare LF inside the trailer */
        "0\r\nX: a\rY: b\r\n\r\n",                       /* bare CR inside the trailer */
        "0\r\nX: a\x7f\r\n\r\n",                         /* DEL in a trailer value */
    };
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        size_t decoded_len = 99;
        assert(chunked_body_scan(rejected[i], strlen(rejected[i]), MAX_BODY_SIZE, &decoded_len) == -1);
    }
    static const char nul_in_trailer[] = "0\r\nX: a\0b\r\n\r\n";
    size_t nul_decoded;
    assert(chunked_body_scan(nul_in_trailer, sizeof(nul_in_trailer) - 1, MAX_BODY_SIZE, &nul_decoded) == -1);

    const struct { const char *body; size_t decoded; } accepted[] = {
        {"5\r\nhello\r\n0\r\n\r\n", 5},
        {"05\r\nhello\r\n000\r\n\r\n", 5},
        {"a\r\n0123456789\r\n0\r\n\r\n", 10},
        {"A\r\n0123456789\r\n0\r\n\r\n", 10},
        {"5;ext\r\nhello\r\n0\r\n\r\n", 5},
        {"5 ;ext=\"a b\"\r\nhello\r\n0\r\n\r\n", 5},  /* BWS before ";", HTAB / SP inside the extension */
        {"5\t;ext=1\t\r\nhello\r\n0\r\n\r\n", 5},
        {"0000000000000005\r\nhello\r\n0\r\n\r\n", 5},  /* 16 digits is the most accepted */
        {"0\r\nX: a\tb\r\nY: c\r\n\r\n", 0},
    };
    for (size_t i = 0; i < sizeof(accepted) / sizeof(accepted[0]); i++) {
        size_t decoded_len = 99;
        assert(chunked_body_scan(accepted[i].body, strlen(accepted[i].body), MAX_BODY_SIZE, &decoded_len) == 1);
        assert(decoded_len == accepted[i].decoded);
    }

    /* The whole request from the report: 400, never dispatched with "hello" as its body. */
    const char *heads[] = {"0x5", "+5", " 5"};
    for (size_t i = 0; i < sizeof(heads) / sizeof(heads[0]); i++) {
        char raw[256];
        const int n = snprintf(raw, sizeof(raw), "POST /upload HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n"
                               "Connection: close\r\n\r\n%s\r\nhello\r\n0\r\n\r\n", heads[i]);
        assert(n > 0 && (size_t)n < sizeof(raw));
        assert(request_is_complete(raw, (size_t)n) == 1);
        assert(answer_for(raw, (size_t)n) == 400);
    }
    const char *ok = "POST /upload HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n";
    Request req;
    assert(parse_http_request(ok, strlen(ok), &req, &test_arena) == 0);
    assert(req.content_length == 5 && memcmp(req.body, "hello", 5) == 0);
    arena_reset(&test_arena);
}

/* "every input is answered or closed", at the parser level, for every prefix of each input (every
 * point a recv() could stop at): request_is_complete says "wait" only before any blank line or while a
 * well-framed head's body is genuinely short; once it says "complete" it keeps saying so; the
 * incremental path connection.c takes (head re-parsed per read, chunk_scan carried) agrees at every
 * prefix; and the answer chosen at the first complete prefix is the answer for the whole buffer. The
 * same oracle runs on random inputs in fuzz_parser.c, and end to end in test_answered.c. */
static void test_every_prefix_is_answered_or_waits_for_bytes(void) {
    const char *inputs[] = {
        "GET /a HTTP/1.1\r\nHost: x\r\n\r\n",
        "POST /a HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello",
        "POST /a HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nWiki\r\n5;x=1\r\npedia\r\n0\r\nT: v\r\n\r\n",
        "POST /a HTTP/1.1\r\nContent-Length: 4\r\n\r\n\n\n\r\n",       /* blank lines inside a body */
        "GET /a HTTP/1.1\r\n\r\nGET /b HTTP/1.1\r\n\r\n",
        "GET / X\r\n\r\nGET /a HTTP/1.1\r\n\r\n",
        "GET /a HTTP/1.1\r\nHost: x\n\r\n",
        "GET /a HTTP/2.0\r\n\r\n",
        "POST /a HTTP/1.1\r\nContent-Length: 99999999\r\n\r\n",
        "POST /a HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\n",
        "POST /a HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n\r\n",
        "POST /a HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nab",
        "GET /%00 HTTP/1.1\r\n\r\n",
        "\r\nGET /a HTTP/1.1\r\n\r\n",
        "not http at all\r\n\r\n",
    };
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        const char *in = inputs[i];
        const size_t len = strlen(in);
        size_t first = 0;
        ChunkScanState carried = {0};
        for (size_t k = 1; k <= len; k++) {
            char *exact = malloc(k); /* no NUL, no slack: the parser must stay inside k bytes */
            assert(exact != NULL);
            memcpy(exact, in, k);
            ParsedHead head;
            parse_request_head(exact, k, &head);
            const int complete = request_is_complete(exact, k);
            assert(request_head_is_complete(&head, exact, k, &carried) == complete);
            if (first != 0) {
                assert(complete == 1); /* never "wait" again once the request was complete */
            } else if (complete) {
                first = k;
            } else if (head.header_len == 0) {
                assert(!has_blank_line(exact, k));
            } else if (head.chunked) {
                size_t decoded;
                assert(chunked_body_scan(exact + head.header_len, k - head.header_len, MAX_BODY_SIZE, &decoded) == 0);
            } else {
                assert(head.content_length > 0 && k - head.header_len < (size_t)head.content_length);
            }
            free(exact);
        }
        assert(first != 0); /* every input above is a finished request, well-formed or not */
        assert(answer_for(in, first) == answer_for(in, len));
    }
}

/* A head that trickles in one byte per recv: parse_request_head_resume, fed every prefix with one
 * head_scan carried across them (connection.c's path), gives exactly parse_request_head's answer at
 * every prefix, while head_scan keeps up with the buffer (each byte is searched a bounded number of
 * times, and picohttpparser runs only once a blank line is in) - it used to re-parse from byte 0 on every
 * recv, quadratic in the head size. A head already malformed but not yet terminated waits for its blank
 * line (0), then is rejected (-1) like any other. */
static void test_head_scan_resumes_and_matches_from_scratch(void) {
    static char big[BUF_SIZE];
    size_t n = (size_t)snprintf(big, sizeof(big), "GET /a HTTP/1.1\r\n");
    for (int i = 0; n + 64 < sizeof(big) - 4; i++) {
        n += (size_t)snprintf(big + n, sizeof(big) - n, "X-H%d: 0000000000000000000000\r\n", i);
    }
    memcpy(big + n, "\r\n", 2);
    n += 2;

    const char *inputs[] = {
        big,
        "GET /a HTTP/1.1\r\nHost: x\r\n\r\n",
        "GET /a HTTP/1.1\nHost: x\n\n",                    /* bare-LF blank line: found, then rejected */
        "this is \x01 not a request\r\nX: y\r\n\r\n",  /* malformed long before its blank line */
        "\r\nGET /a HTTP/1.1\r\n\r\n",
    };
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        const size_t len = i == 0 ? n : strlen(inputs[i]);
        size_t head_scan = 0;
        for (size_t k = 1; k <= len; k++) {
            char *exact = malloc(k); /* no NUL, no slack */
            assert(exact != NULL);
            memcpy(exact, inputs[i], k);
            ParsedHead fresh;
            ParsedHead resumed;
            const int want = parse_request_head(exact, k, &fresh);
            assert(parse_request_head_resume(exact, k, &resumed, &head_scan) == want);
            assert(resumed.header_len == fresh.header_len);
            assert(resumed.content_length == fresh.content_length);
            assert(head_scan <= k);
            if (fresh.header_len == 0 && fresh.content_length == 0) {
                assert(!has_blank_line(exact, k));
                assert(k < 2 || head_scan == k - 2); /* the next search starts at the new bytes */
            }
            free(exact);
        }
    }

    /* malformed, unterminated: waits; the blank line then gets the same -1 a single read would. */
    const char *bad = "GET / HTTP/9\x01\r\nX: y\r\n";
    ParsedHead head;
    size_t scan = 0;
    assert(parse_request_head_resume(bad, strlen(bad), &head, &scan) == 0);
    assert(head.header_len == 0 && head.content_length == 0);
    assert(request_is_complete(bad, strlen(bad)) == 0);
    const char *bad_done = "GET / HTTP/9\x01\r\nX: y\r\n\r\n";
    assert(parse_request_head_resume(bad_done, strlen(bad_done), &head, &scan) == -1);
    assert(request_is_complete(bad_done, strlen(bad_done)) == 1);
}

static void check_canonical(const char *in, const int rc, const char *out) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", in);
    assert(path_canonicalize(buf) == rc);
    if (rc == 0) {
        assert(strcmp(buf, out) == 0);
    }
}

static void test_path_canonicalize(void) {
    /* the router skips empty segments, so the prefix-middleware check must see the same shape. */
    check_canonical("/", 0, "/");
    check_canonical("//", 0, "/");
    check_canonical("/admin/secret", 0, "/admin/secret");
    check_canonical("//admin/secret", 0, "/admin/secret");
    check_canonical("/admin//secret//", 0, "/admin/secret/");
    check_canonical("///a///b", 0, "/a/b");
    check_canonical("*", 0, "*");
    check_canonical("/.well-known/x", 0, "/.well-known/x"); /* a dot-prefixed name is not a dot segment */
    check_canonical("/a/..b/c.", 0, "/a/..b/c.");
    /* Dot segments and non-origin-form targets are refused, not resolved. */
    check_canonical("/./admin", -1, NULL);
    check_canonical("/x/../admin", -1, NULL);
    check_canonical("/admin/.", -1, NULL);
    check_canonical("/admin/..", -1, NULL);
    check_canonical("admin/secret", -1, NULL);
    check_canonical("http://x/admin", -1, NULL);
    check_canonical("", -1, NULL);
}

static void test_path_prefix_helpers(void) {
    char out[128];
    path_normalize_prefix("/admin", out, sizeof(out));
    assert(strcmp(out, "/admin") == 0);
    path_normalize_prefix("/admin/", out, sizeof(out)); /* used to match no sub-path at all */
    assert(strcmp(out, "/admin") == 0);
    path_normalize_prefix("admin", out, sizeof(out));
    assert(strcmp(out, "/admin") == 0);
    path_normalize_prefix("//a//b/", out, sizeof(out));
    assert(strcmp(out, "/a/b") == 0);
    path_normalize_prefix("/", out, sizeof(out));
    assert(strcmp(out, "") == 0);
    path_normalize_prefix(NULL, out, sizeof(out));
    assert(strcmp(out, "") == 0);
    char small[8];
    path_normalize_prefix("/abc/defgh", small, sizeof(small)); /* truncates at a segment boundary */
    assert(strcmp(small, "/abc") == 0);

    assert(path_prefix_matches("", "/anything") == 1);
    assert(path_prefix_matches("/admin", "/admin") == 1);
    assert(path_prefix_matches("/admin", "/admin/") == 1);
    assert(path_prefix_matches("/admin", "/admin/x") == 1);
    assert(path_prefix_matches("/admin", "/adminx") == 0);
    assert(path_prefix_matches("/admin", "/") == 0);
}

static void test_request_path_is_canonical(void) {
    Request req;
    const char *doubled = "GET //admin//secret HTTP/1.1\r\nHost: x\r\n\r\n";
    assert(parse_http_request(doubled, strlen(doubled), &req, &test_arena) == 0);
    assert(strcmp(req.path, "/admin/secret") == 0);
    arena_reset(&test_arena);

    const char *rejected[] = {
        "GET /%2Fadmin/secret HTTP/1.1\r\nHost: x\r\n\r\n",  /* encoded '/' would create a segment */
        "GET /%2fadmin/secret HTTP/1.1\r\nHost: x\r\n\r\n",
        "GET /admin%2Fsecret HTTP/1.1\r\nHost: x\r\n\r\n",
        "GET /x/../admin/secret HTTP/1.1\r\nHost: x\r\n\r\n",
        "GET /x/%2e%2e/admin/secret HTTP/1.1\r\nHost: x\r\n\r\n", /* dot segment after decoding */
        "GET /./admin/secret HTTP/1.1\r\nHost: x\r\n\r\n",
        "GET admin/secret HTTP/1.1\r\nHost: x\r\n\r\n",
        "GET http://x/admin/secret HTTP/1.1\r\nHost: x\r\n\r\n",
    };
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        assert(parse_http_request(rejected[i], strlen(rejected[i]), &req, &test_arena) == -4);
        arena_reset(&test_arena);
    }
}

int main(void) {
    arena_init(&test_arena, test_arena_buf, sizeof(test_arena_buf));
    test_framing_is_line_anchored();
    test_content_length_is_strict();
    test_request_framing();
    test_request_framing_path_out();
    test_parse_http_request_header_views_are_not_capped();
    test_wants_close_header_forms();
    test_request_line_limits();
    test_parse_does_not_depend_on_zeroed_request();
    test_header_value_whitespace_and_limits();
    test_percent_decoding_at_boundaries();
    test_embedded_nul_rejected();
    test_malformed_request_line_rejected();
    test_bare_lf_rejected();
    test_transfer_encoding_token_matching();
    test_short_version_token_rejected();
    test_every_prefix_is_answered_or_waits_for_bytes();
    test_head_scan_resumes_and_matches_from_scratch();
    test_chunk_size_line_is_strict();
    test_path_canonicalize();
    test_path_prefix_helpers();
    test_request_path_is_canonical();
    printf("all http hardening tests passed\n");
    return 0;
}
