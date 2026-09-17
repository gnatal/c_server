#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_types.h"
#include "http_parser.h"

static void test_extract_content_length(void) {
    /* Absent Content-Length returns 0 */
    assert(extract_content_length("Host: localhost\r\nAccept: */*\r\n") == 0);

    /* Standard valid Content-Length */
    assert(extract_content_length("Content-Length: 42\r\n") == 42);
    assert(extract_content_length("Host: localhost\r\nContent-Length: 0\r\n") == 0);

    /* Case-insensitivity */
    assert(extract_content_length("content-length: 123\r\n") == 123);
    assert(extract_content_length("CONTENT-LENGTH: 99\r\n") == 99);

    /* Negative value: malformed, sentinel -1 (-> 400 Bad Request) */
    assert(extract_content_length("Content-Length: -1\r\n") == -1);
    assert(extract_content_length("Content-Length: -100\r\n") == -1);

    /* A body well beyond the old BUF_SIZE is fine now - body storage is
     * decoupled from BUF_SIZE (see lib/CLAUDE.md, "Body buffering"). */
    assert(extract_content_length("Content-Length: 8192\r\n") == 8192);
    assert(extract_content_length("Content-Length: 100000\r\n") == 100000);

    /* Exceeding MAX_BODY_SIZE: well-formed but too large, distinct sentinel
     * -2 (-> 413 Payload Too Large, not 400 - see connection.c) */
    char over[64];
    snprintf(over, sizeof(over), "Content-Length: %d\r\n", MAX_BODY_SIZE + 1);
    assert(extract_content_length(over) == -2);

    char way_over[64];
    snprintf(way_over, sizeof(way_over), "Content-Length: %d\r\n", MAX_BODY_SIZE * 4);
    assert(extract_content_length(way_over) == -2);

    /* Boundary: exact max allowed MAX_BODY_SIZE */
    char at_max[64];
    snprintf(at_max, sizeof(at_max), "Content-Length: %d\r\n", MAX_BODY_SIZE);
    assert(extract_content_length(at_max) == MAX_BODY_SIZE);
}

static void test_request_is_complete(void) {
    /* Incomplete headers */
    const char *partial_head = "GET /index.html HTTP/1.1\r\nHost: local";
    assert(request_is_complete(partial_head, strlen(partial_head)) == 0);

    /* Headers complete, no body / no Content-Length */
    const char *complete_head = "GET /index.html HTTP/1.1\r\nHost: localhost\r\n\r\n";
    assert(request_is_complete(complete_head, strlen(complete_head)) == 1);

    /* Headers complete, Content-Length declared but body incomplete */
    const char *partial_body = "POST /api HTTP/1.1\r\nContent-Length: 10\r\n\r\n12345";
    assert(request_is_complete(partial_body, strlen(partial_body)) == 0);

    /* Headers complete, Content-Length matches body length */
    const char *exact_body = "POST /api HTTP/1.1\r\nContent-Length: 5\r\n\r\n12345";
    assert(request_is_complete(exact_body, strlen(exact_body)) == 1);

    /* Headers complete, Content-Length smaller than body (excess received) */
    const char *excess_body = "POST /api HTTP/1.1\r\nContent-Length: 5\r\n\r\n123456789";
    assert(request_is_complete(excess_body, strlen(excess_body)) == 1);

    /* Invalid Content-Length returns 1 to halt buffering and fail in parser */
    const char *invalid_cl = "POST /api HTTP/1.1\r\nContent-Length: -1\r\n\r\n";
    assert(request_is_complete(invalid_cl, strlen(invalid_cl)) == 1);
}

static void test_request_wants_close(void) {
    Request req;

    /* Explicit Connection: close */
    memset(&req, 0, sizeof(req));
    strncpy(req.version, "HTTP/1.1", sizeof(req.version) - 1);
    strncpy(req.headers, "Host: localhost\r\nConnection: close\r\n", sizeof(req.headers) - 1);
    assert(request_wants_close(&req) == 1);

    /* Explicit Connection: keep-alive overrides HTTP/1.0 */
    memset(&req, 0, sizeof(req));
    strncpy(req.version, "HTTP/1.0", sizeof(req.version) - 1);
    strncpy(req.headers, "Host: localhost\r\nConnection: keep-alive\r\n", sizeof(req.headers) - 1);
    assert(request_wants_close(&req) == 0);

    /* HTTP/1.1 default without Connection header is keep-alive */
    memset(&req, 0, sizeof(req));
    strncpy(req.version, "HTTP/1.1", sizeof(req.version) - 1);
    strncpy(req.headers, "Host: localhost\r\n", sizeof(req.headers) - 1);
    assert(request_wants_close(&req) == 0);

    /* HTTP/1.0 default without Connection header is close */
    memset(&req, 0, sizeof(req));
    strncpy(req.version, "HTTP/1.0", sizeof(req.version) - 1);
    strncpy(req.headers, "Host: localhost\r\n", sizeof(req.headers) - 1);
    assert(request_wants_close(&req) == 1);

    /* Non-HTTP/1.1 version defaults to close */
    memset(&req, 0, sizeof(req));
    strncpy(req.version, "HTTP/0.9", sizeof(req.version) - 1);
    assert(request_wants_close(&req) == 1);
}

static void test_parse_http_request(void) {
    Request req;

    /* Simple GET without query */
    const char *raw_get = "GET /items HTTP/1.1\r\nHost: example.com\r\nAccept: */*\r\n\r\n";
    assert(parse_http_request(raw_get, strlen(raw_get), &req) == 0);
    assert(strcmp(req.method, "GET") == 0);
    assert(strcmp(req.path, "/items") == 0);
    assert(strcmp(req.query, "") == 0);
    assert(strcmp(req.version, "HTTP/1.1") == 0);
    assert(req.content_length == 0);
    assert(req.body != NULL);
    assert(strcmp(req.body, "") == 0);
    assert(strstr(req.headers, "Host: example.com") != NULL);
    free(req.body);

    /* Request line with zero header lines before the blank-line terminator
     * (request-line CRLF immediately followed by the blank-line CRLF).
     * Regression test: this used to make the request-line's own "\r\n"
     * coincide with the start of "\r\n\r\n", producing a negative
     * header_start-to-header_end distance that wrapped to a huge size_t and
     * caused an out-of-bounds heap read in the memcpy into req->headers
     * (found via ASan) - a header-less request is unusual but perfectly
     * legal to receive and must not crash/over-read. */
    const char *raw_no_headers = "GET /ping HTTP/1.1\r\n\r\n";
    assert(parse_http_request(raw_no_headers, strlen(raw_no_headers), &req) == 0);
    assert(strcmp(req.path, "/ping") == 0);
    assert(strcmp(req.headers, "") == 0);
    assert(req.header_count == 0);
    free(req.body);

    /* GET with query string */
    const char *raw_query = "GET /search?q=test&page=2 HTTP/1.1\r\nHost: example.com\r\n\r\n";
    assert(parse_http_request(raw_query, strlen(raw_query), &req) == 0);
    assert(strcmp(req.method, "GET") == 0);
    assert(strcmp(req.path, "/search") == 0);
    assert(strcmp(req.query, "q=test&page=2") == 0);
    /* parse_http_request also populates the parsed query-param arrays. */
    assert(strcmp(req_get_query(&req, "q"), "test") == 0);
    assert(strcmp(req_get_query(&req, "page"), "2") == 0);
    free(req.body);

    /* Percent-encoded path segment is URL-decoded; req->query stays raw
     * (unparsed), only the parsed query-param arrays are decoded. */
    const char *raw_encoded = "GET /a%20b/caf%C3%A9?name=a%20b HTTP/1.1\r\nHost: example.com\r\n\r\n";
    assert(parse_http_request(raw_encoded, strlen(raw_encoded), &req) == 0);
    assert(strcmp(req.path, "/a b/caf\xC3\xA9") == 0);
    assert(strcmp(req.query, "name=a%20b") == 0);
    assert(strcmp(req_get_query(&req, "name"), "a b") == 0);
    free(req.body);

    /* POST with body and Content-Length */
    const char *raw_post =
        "POST /echo HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 13\r\n"
        "\r\n"
        "Hello, World!";
    assert(parse_http_request(raw_post, strlen(raw_post), &req) == 0);
    assert(strcmp(req.method, "POST") == 0);
    assert(strcmp(req.path, "/echo") == 0);
    assert(req.content_length == 13);
    assert(strcmp(req.body, "Hello, World!") == 0);
    /* parse_http_request also populates the parsed header map. */
    assert(strcmp(req_get_header(&req, "Host"), "localhost") == 0);
    assert(strcmp(req_get_header(&req, "content-length"), "13") == 0);
    assert(req_get_header(&req, "X-Missing") == NULL);
    free(req.body);

    /* parse_http_request also populates the parsed cookie map, off the
     * "Cookie" header (case-insensitive lookup finds it via req_get_header,
     * cookie name lookup itself stays case-sensitive per RFC 6265). */
    const char *raw_with_cookies =
        "GET /profile HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Cookie: session=abc123; theme=dark\r\n"
        "\r\n";
    assert(parse_http_request(raw_with_cookies, strlen(raw_with_cookies), &req) == 0);
    assert(strcmp(req_get_cookie(&req, "session"), "abc123") == 0);
    assert(strcmp(req_get_cookie(&req, "theme"), "dark") == 0);
    assert(req_get_cookie(&req, "missing") == NULL);
    free(req.body);

    /* No Cookie header at all -> zero parsed cookies, not a parse error. */
    const char *raw_no_cookies = "GET /profile HTTP/1.1\r\nHost: localhost\r\n\r\n";
    assert(parse_http_request(raw_no_cookies, strlen(raw_no_cookies), &req) == 0);
    assert(req.cookie_count == 0);
    free(req.body);

    /* A body well beyond the old BUF_SIZE (8192) now parses cleanly -
     * req->body is heap-allocated to fit it exactly rather than being
     * copied into a BUF_SIZE-capped fixed array (see lib/CLAUDE.md,
     * "Body buffering"). */
    const size_t big_len = 20000;
    char *big_body = malloc(big_len + 1);
    memset(big_body, 'x', big_len);
    big_body[big_len] = '\0';
    char big_head[128];
    snprintf(big_head, sizeof(big_head), "POST /upload HTTP/1.1\r\nContent-Length: %zu\r\n\r\n", big_len);
    size_t big_head_len = strlen(big_head);
    char *big_raw = malloc(big_head_len + big_len + 1);
    memcpy(big_raw, big_head, big_head_len);
    memcpy(big_raw + big_head_len, big_body, big_len + 1);
    assert(parse_http_request(big_raw, big_head_len + big_len, &req) == 0);
    assert(req.content_length == (int)big_len);
    assert(strlen(req.body) == big_len);
    assert(strcmp(req.body, big_body) == 0);
    free(req.body);
    free(big_raw);
    free(big_body);

    /* Malformed request line */
    const char *malformed_line = "INVALID\r\n\r\n";
    assert(parse_http_request(malformed_line, strlen(malformed_line), &req) == -1);

    /* Missing header terminator */
    const char *no_term = "GET / HTTP/1.1\r\nHost: example.com";
    assert(parse_http_request(no_term, strlen(no_term), &req) == -1);

    /* Negative Content-Length */
    const char *bad_cl = "POST / HTTP/1.1\r\nContent-Length: -5\r\n\r\nbody";
    assert(parse_http_request(bad_cl, strlen(bad_cl), &req) == -1);

    /* Content-Length exceeding MAX_BODY_SIZE is still rejected, just at a
     * much higher ceiling than the old BUF_SIZE-based one. */
    char over_head[64];
    snprintf(over_head, sizeof(over_head), "POST / HTTP/1.1\r\nContent-Length: %d\r\n\r\nbody", MAX_BODY_SIZE + 1);
    assert(parse_http_request(over_head, strlen(over_head), &req) == -1);

    /* A request-line path longer than req->path (256 bytes) can hold is
     * rejected with the distinct -2 sentinel (-> 414 URI Too Long,
     * connection.c) rather than being silently truncated into a shorter
     * path the client never asked for. 300 chars is comfortably under the
     * 511-char sscanf token cap in parse_http_request, but well past 255. */
    const size_t long_path_len = 300;
    char *long_path = malloc(long_path_len + 1);
    long_path[0] = '/';
    memset(long_path + 1, 'a', long_path_len - 1);
    long_path[long_path_len] = '\0';
    char *long_raw = malloc(4 + long_path_len + 13 + 1);
    snprintf(long_raw, 4 + long_path_len + 13 + 1, "GET %s HTTP/1.1\r\n\r\n", long_path);
    assert(parse_http_request(long_raw, strlen(long_raw), &req) == -2);
    free(long_path);
    free(long_raw);

    /* A path that just fits (255 chars, one under the 256-byte cap) still
     * parses normally. */
    const size_t fit_path_len = 255;
    char *fit_path = malloc(fit_path_len + 1);
    fit_path[0] = '/';
    memset(fit_path + 1, 'a', fit_path_len - 1);
    fit_path[fit_path_len] = '\0';
    char *fit_raw = malloc(4 + fit_path_len + 13 + 1);
    snprintf(fit_raw, 4 + fit_path_len + 13 + 1, "GET %s HTTP/1.1\r\n\r\n", fit_path);
    assert(parse_http_request(fit_raw, strlen(fit_raw), &req) == 0);
    assert(strlen(req.path) == fit_path_len);
    free(req.body);
    free(fit_path);
    free(fit_raw);

    /* Binary-safe body: a body containing an embedded NUL byte (e.g. a
     * binary file inside a multipart/form-data part, lib/multipart.h) must
     * be copied in full, not truncated at the first NUL the way
     * strlen(body_start) would - raw_len (not strlen(raw)) is what bounds
     * how many body bytes parse_http_request copies. */
    const char binary_body[6] = { 'a', 'b', '\0', 'c', 'd', 'e' };
    char binary_head[64];
    snprintf(binary_head, sizeof(binary_head), "POST /upload HTTP/1.1\r\nContent-Length: %d\r\n\r\n",
             (int)sizeof(binary_body));
    size_t binary_head_len = strlen(binary_head);
    char *binary_raw = malloc(binary_head_len + sizeof(binary_body));
    memcpy(binary_raw, binary_head, binary_head_len);
    memcpy(binary_raw + binary_head_len, binary_body, sizeof(binary_body));
    assert(parse_http_request(binary_raw, binary_head_len + sizeof(binary_body), &req) == 0);
    assert(req.content_length == (int)sizeof(binary_body));
    assert(memcmp(req.body, binary_body, sizeof(binary_body)) == 0);
    free(req.body);
    free(binary_raw);
}

static void test_request_has_chunked_encoding(void) {
    assert(request_has_chunked_encoding("Transfer-Encoding: chunked\r\n") == 1);
    /* Case-insensitive, same as every other header lookup in this engine. */
    assert(request_has_chunked_encoding("transfer-encoding: CHUNKED\r\n") == 1);
    /* Absent header. */
    assert(request_has_chunked_encoding("Host: localhost\r\n") == 0);
    /* Present but a different encoding. */
    assert(request_has_chunked_encoding("Transfer-Encoding: gzip\r\n") == 0);
    /* "chunked" appearing elsewhere must not cause a false match - only the
     * Transfer-Encoding header's own line is scanned. */
    assert(request_has_chunked_encoding(
        "Transfer-Encoding: gzip\r\nX-Debug: chunked-test\r\n\r\n") == 0);
    /* A raw connection buffer (headers + whatever body has arrived) is a
     * valid input too - the same dual-use extract_content_length has. */
    assert(request_has_chunked_encoding(
        "POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nWiki\r\n0\r\n\r\n") == 1);
}

static void test_chunked_body_scan(void) {
    size_t decoded_len;

    /* Single chunk, no trailers. */
    const char *one_chunk = "4\r\nWiki\r\n0\r\n\r\n";
    assert(chunked_body_scan(one_chunk, strlen(one_chunk), MAX_BODY_SIZE, &decoded_len) == 1);
    assert(decoded_len == 4);

    /* Multiple chunks. */
    const char *two_chunks = "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
    assert(chunked_body_scan(two_chunks, strlen(two_chunks), MAX_BODY_SIZE, &decoded_len) == 1);
    assert(decoded_len == 9);

    /* Chunk extensions are accepted and ignored. */
    const char *with_ext = "4;ext=1\r\nWiki\r\n0\r\n\r\n";
    assert(chunked_body_scan(with_ext, strlen(with_ext), MAX_BODY_SIZE, &decoded_len) == 1);
    assert(decoded_len == 4);

    /* Trailer headers after the last-chunk are accepted (and, per
     * chunked_body_decode, discarded rather than surfaced to the caller). */
    const char *with_trailer = "0\r\nExpires: never\r\n\r\n";
    assert(chunked_body_scan(with_trailer, strlen(with_trailer), MAX_BODY_SIZE, &decoded_len) == 1);
    assert(decoded_len == 0);

    /* Incomplete: chunk data hasn't fully arrived yet. */
    const char *partial_data = "4\r\nWi";
    assert(chunked_body_scan(partial_data, strlen(partial_data), MAX_BODY_SIZE, &decoded_len) == 0);
    assert(decoded_len == 0);

    /* Incomplete: a whole chunk arrived, but not the terminating last-chunk. */
    const char *partial_terminator = "4\r\nWiki\r\n";
    assert(chunked_body_scan(partial_terminator, strlen(partial_terminator), MAX_BODY_SIZE, &decoded_len) == 0);
    assert(decoded_len == 4);

    /* Incomplete: chunk-size line itself hasn't fully arrived. */
    const char *partial_size_line = "4";
    assert(chunked_body_scan(partial_size_line, strlen(partial_size_line), MAX_BODY_SIZE, &decoded_len) == 0);

    /* Malformed: non-hex chunk-size. */
    const char *bad_hex = "ZZ\r\nWiki\r\n0\r\n\r\n";
    assert(chunked_body_scan(bad_hex, strlen(bad_hex), MAX_BODY_SIZE, &decoded_len) == -1);

    /* Malformed: chunk data not followed by its own CRLF (a chunk-size that
     * doesn't match the actual data would otherwise desync every later
     * chunk boundary). */
    const char *bad_terminator = "4\r\nWikiXX0\r\n\r\n";
    assert(chunked_body_scan(bad_terminator, strlen(bad_terminator), MAX_BODY_SIZE, &decoded_len) == -1);

    /* Malformed: an empty chunk-size line. */
    const char *empty_size_line = "\r\nWiki\r\n0\r\n\r\n";
    assert(chunked_body_scan(empty_size_line, strlen(empty_size_line), MAX_BODY_SIZE, &decoded_len) == -1);

    /* Malformed: chunk-size line far longer than any legitimate one, with
     * no CRLF in sight - rejected outright rather than buffered forever. */
    char huge_line[256];
    memset(huge_line, 'a', sizeof(huge_line) - 1);
    huge_line[sizeof(huge_line) - 1] = '\0';
    assert(chunked_body_scan(huge_line, strlen(huge_line), MAX_BODY_SIZE, &decoded_len) == -1);

    /* Oversized: the declared chunk size alone already exceeds the cap -
     * rejected without needing that much data to actually arrive. */
    const char *too_big = "A\r\n";
    assert(chunked_body_scan(too_big, strlen(too_big), 5, &decoded_len) == -2);

    /* Oversized: the *cumulative* decoded size across multiple confirmed
     * chunks exceeds the cap, even though no single chunk alone does. */
    const char *cumulative_too_big = "4\r\nWiki\r\n4\r\npedi\r\n0\r\n\r\n";
    assert(chunked_body_scan(cumulative_too_big, strlen(cumulative_too_big), 6, &decoded_len) == -2);
}

static void test_chunked_body_decode(void) {
    const char *two_chunks = "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
    char out[16] = { 0 };
    size_t written = chunked_body_decode(two_chunks, strlen(two_chunks), out);
    assert(written == 9);
    assert(memcmp(out, "Wikipedia", 9) == 0);

    /* Trailer headers are discarded, not appended to the decoded body. */
    const char *with_trailer = "4\r\nWiki\r\n0\r\nExpires: never\r\n\r\n";
    written = chunked_body_decode(with_trailer, strlen(with_trailer), out);
    assert(written == 4);
    assert(memcmp(out, "Wiki", 4) == 0);

    /* Decoded chunk data can contain arbitrary bytes, including embedded
     * NULs (e.g. a chunked binary upload) - the return value, not strlen(),
     * is what callers must rely on. */
    const char chunked_binary[] = "3\r\na\0b\r\n0\r\n\r\n";
    written = chunked_body_decode(chunked_binary, sizeof(chunked_binary) - 1, out);
    assert(written == 3);
    assert(memcmp(out, "a\0b", 3) == 0);
}

static void test_request_is_complete_chunked(void) {
    /* Complete chunked request. */
    const char *complete = "POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nWiki\r\n0\r\n\r\n";
    assert(request_is_complete(complete, strlen(complete)) == 1);

    /* Incomplete: last-chunk terminator hasn't arrived yet. */
    const char *incomplete = "POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nWiki\r\n";
    assert(request_is_complete(incomplete, strlen(incomplete)) == 0);

    /* Malformed chunk framing stops buffering (like an invalid
     * Content-Length does) so parse_http_request can reject it as a 400
     * rather than waiting on bytes that will never form a valid body. */
    const char *malformed = "POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\nZZ\r\nWiki\r\n0\r\n\r\n";
    assert(request_is_complete(malformed, strlen(malformed)) == 1);

    /* A non-chunked request with no matching Transfer-Encoding header still
     * uses ordinary Content-Length-based completeness (regression guard for
     * the branch added alongside chunked support). */
    const char *plain = "POST /x HTTP/1.1\r\nContent-Length: 5\r\n\r\n12345";
    assert(request_is_complete(plain, strlen(plain)) == 1);
}

static void test_parse_http_request_chunked(void) {
    Request req;

    /* Basic chunked POST, multiple chunks. */
    const char *raw = "POST /echo HTTP/1.1\r\nHost: example.com\r\n"
                       "Transfer-Encoding: chunked\r\n\r\n"
                       "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
    assert(parse_http_request(raw, strlen(raw), &req) == 0);
    assert(strcmp(req.method, "POST") == 0);
    assert(strcmp(req.path, "/echo") == 0);
    assert(req.content_length == 9);
    assert(strcmp(req.body, "Wikipedia") == 0);
    free(req.body);

    /* Trailer headers are accepted but not surfaced anywhere on Request. */
    const char *with_trailer = "POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
                                "4\r\nWiki\r\n0\r\nExpires: never\r\n\r\n";
    assert(parse_http_request(with_trailer, strlen(with_trailer), &req) == 0);
    assert(strcmp(req.body, "Wiki") == 0);
    free(req.body);

    /* A chunked body can contain embedded NUL bytes - the caller must rely
     * on req.content_length, not strlen(req.body), same as the equivalent
     * Content-Length case (see the binary-body case above). */
    const char chunked_binary_head[] = "POST /upload HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";
    const char chunked_binary_body[] = "3\r\na\0b\r\n0\r\n\r\n";
    char binary_raw[128];
    size_t head_len = strlen(chunked_binary_head);
    size_t body_len = sizeof(chunked_binary_body) - 1;
    memcpy(binary_raw, chunked_binary_head, head_len);
    memcpy(binary_raw + head_len, chunked_binary_body, body_len);
    assert(parse_http_request(binary_raw, head_len + body_len, &req) == 0);
    assert(req.content_length == 3);
    assert(memcmp(req.body, "a\0b", 3) == 0);
    free(req.body);

    /* RFC 7230 3.3.3: Transfer-Encoding and Content-Length together is an
     * ambiguous/smuggling-shaped message - rejected outright (400), not
     * resolved by preferring one header over the other. */
    const char *both_headers = "POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n"
                                "Content-Length: 4\r\n\r\n4\r\nWiki\r\n0\r\n\r\n";
    assert(parse_http_request(both_headers, strlen(both_headers), &req) == -1);
    assert(req.body == NULL);

    /* Malformed chunk framing fails the parse (-1), same status family a
     * malformed Content-Length gets. */
    const char *malformed = "POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\nZZ\r\nWiki\r\n0\r\n\r\n";
    assert(parse_http_request(malformed, strlen(malformed), &req) == -1);
    assert(req.body == NULL);
}

static void test_status_text(void) {
    assert(strcmp(status_text(200), "OK") == 0);
    assert(strcmp(status_text(201), "Created") == 0);
    assert(strcmp(status_text(204), "No Content") == 0);
    assert(strcmp(status_text(206), "Partial Content") == 0);
    assert(strcmp(status_text(301), "Moved Permanently") == 0);
    assert(strcmp(status_text(302), "Found") == 0);
    assert(strcmp(status_text(303), "See Other") == 0);
    assert(strcmp(status_text(304), "Not Modified") == 0);
    assert(strcmp(status_text(307), "Temporary Redirect") == 0);
    assert(strcmp(status_text(308), "Permanent Redirect") == 0);
    assert(strcmp(status_text(400), "Bad Request") == 0);
    assert(strcmp(status_text(401), "Unauthorized") == 0);
    assert(strcmp(status_text(403), "Forbidden") == 0);
    assert(strcmp(status_text(404), "Not Found") == 0);
    assert(strcmp(status_text(405), "Method Not Allowed") == 0);
    assert(strcmp(status_text(408), "Request Timeout") == 0);
    assert(strcmp(status_text(409), "Conflict") == 0);
    assert(strcmp(status_text(410), "Gone") == 0);
    assert(strcmp(status_text(413), "Payload Too Large") == 0);
    assert(strcmp(status_text(414), "URI Too Long") == 0);
    assert(strcmp(status_text(415), "Unsupported Media Type") == 0);
    assert(strcmp(status_text(422), "Unprocessable Entity") == 0);
    assert(strcmp(status_text(429), "Too Many Requests") == 0);
    assert(strcmp(status_text(431), "Request Header Fields Too Large") == 0);
    assert(strcmp(status_text(500), "Internal Server Error") == 0);
    assert(strcmp(status_text(501), "Not Implemented") == 0);
    assert(strcmp(status_text(502), "Bad Gateway") == 0);
    assert(strcmp(status_text(503), "Service Unavailable") == 0);
    assert(strcmp(status_text(418), "Unknown") == 0);
    assert(strcmp(status_text(0), "Unknown") == 0);
}

static void test_url_decode(void) {
    char dst[64];

    /* Plain string with nothing to decode */
    url_decode("hello", dst, sizeof(dst), 0);
    assert(strcmp(dst, "hello") == 0);

    /* %XX hex escapes */
    url_decode("a%20b%2Fc", dst, sizeof(dst), 0);
    assert(strcmp(dst, "a b/c") == 0);

    /* '+' only decodes to space when decode_plus is set */
    url_decode("a+b", dst, sizeof(dst), 0);
    assert(strcmp(dst, "a+b") == 0);
    url_decode("a+b", dst, sizeof(dst), 1);
    assert(strcmp(dst, "a b") == 0);

    /* Malformed escape (missing/short hex digits) passes through literally */
    url_decode("100%done", dst, sizeof(dst), 0);
    assert(strcmp(dst, "100%done") == 0);
    url_decode("trailing%", dst, sizeof(dst), 0);
    assert(strcmp(dst, "trailing%") == 0);
    url_decode("bad%2gvalue", dst, sizeof(dst), 0);
    assert(strcmp(dst, "bad%2gvalue") == 0);

    /* In-place decode (dst == src) is safe since output never grows */
    char inplace[32];
    strncpy(inplace, "a%20b%20c", sizeof(inplace) - 1);
    inplace[sizeof(inplace) - 1] = '\0';
    url_decode(inplace, inplace, sizeof(inplace), 0);
    assert(strcmp(inplace, "a b c") == 0);

    /* dst_size truncates rather than overflowing */
    char small[4];
    url_decode("abcdef", small, sizeof(small), 0);
    assert(strcmp(small, "abc") == 0);
}

static void test_parse_headers(void) {
    Request req;
    memset(&req, 0, sizeof(req));

    /* Empty header block yields zero headers */
    parse_headers("", &req);
    assert(req.header_count == 0);
    assert(req_get_header(&req, "Host") == NULL);

    /* Basic Name: value pairs, leading space after ':' trimmed */
    parse_headers("Host: example.com\r\nContent-Type: application/json", &req);
    assert(req.header_count == 2);
    assert(strcmp(req_get_header(&req, "Host"), "example.com") == 0);
    assert(strcmp(req_get_header(&req, "Content-Type"), "application/json") == 0);

    /* Lookup is case-insensitive on the header name (RFC 7230) */
    assert(strcmp(req_get_header(&req, "host"), "example.com") == 0);
    assert(strcmp(req_get_header(&req, "CONTENT-TYPE"), "application/json") == 0);

    /* A line with no ':' is skipped rather than stored */
    parse_headers("Host: example.com\r\nMalformedLine\r\nAccept: */*", &req);
    assert(req.header_count == 2);
    assert(strcmp(req_get_header(&req, "Accept"), "*/*") == 0);

    /* Duplicate header: lookup returns the first occurrence */
    parse_headers("X-Token: first\r\nX-Token: second", &req);
    assert(req.header_count == 2);
    assert(strcmp(req_get_header(&req, "X-Token"), "first") == 0);

    /* A value with no leading space after ':' is still captured as-is */
    parse_headers("X-Flag:on", &req);
    assert(strcmp(req_get_header(&req, "X-Flag"), "on") == 0);

    /* Overflow: headers past MAX_HEADERS are dropped, not overflowed. */
    char many[1024] = "";
    for (int i = 0; i < MAX_HEADERS + 5; i++) {
        char line[32];
        snprintf(line, sizeof(line), "H%d: %d\r\n", i, i);
        strncat(many, line, sizeof(many) - strlen(many) - 1);
    }
    parse_headers(many, &req);
    assert(req.header_count == MAX_HEADERS);
}

static void test_parse_cookies(void) {
    Request req;
    memset(&req, 0, sizeof(req));

    /* NULL (no Cookie header present) and empty string both yield zero cookies. */
    parse_cookies(NULL, &req);
    assert(req.cookie_count == 0);
    assert(req_get_cookie(&req, "session") == NULL);

    parse_cookies("", &req);
    assert(req.cookie_count == 0);

    /* Basic "a=1; b=2" pairs, leading space after ';' trimmed. */
    parse_cookies("session=abc123; theme=dark", &req);
    assert(req.cookie_count == 2);
    assert(strcmp(req_get_cookie(&req, "session"), "abc123") == 0);
    assert(strcmp(req_get_cookie(&req, "theme"), "dark") == 0);

    /* Cookie name lookup is case-sensitive (RFC 6265), unlike req_get_header. */
    assert(req_get_cookie(&req, "Session") == NULL);

    /* Not URL-decoded - unlike query-string values. */
    parse_cookies("greeting=hello%20world", &req);
    assert(strcmp(req_get_cookie(&req, "greeting"), "hello%20world") == 0);

    /* A pair with no '=' is malformed - skipped rather than stored. */
    parse_cookies("a=1; malformed; b=2", &req);
    assert(req.cookie_count == 2);
    assert(strcmp(req_get_cookie(&req, "a"), "1") == 0);
    assert(strcmp(req_get_cookie(&req, "b"), "2") == 0);

    /* Overflow: pairs past MAX_COOKIES are dropped, not overflowed. */
    char many[512] = "";
    for (int i = 0; i < MAX_COOKIES + 5; i++) {
        char pair[16];
        snprintf(pair, sizeof(pair), "c%d=%d; ", i, i);
        strncat(many, pair, sizeof(many) - strlen(many) - 1);
    }
    parse_cookies(many, &req);
    assert(req.cookie_count == MAX_COOKIES);
}

static void test_parse_query_string(void) {
    Request req;
    memset(&req, 0, sizeof(req));

    /* Empty query string yields zero pairs */
    parse_query_string("", &req);
    assert(req.query_count == 0);
    assert(req_get_query(&req, "q") == NULL);

    /* Basic key=value pairs */
    parse_query_string("q=cats&page=2", &req);
    assert(req.query_count == 2);
    assert(strcmp(req_get_query(&req, "q"), "cats") == 0);
    assert(strcmp(req_get_query(&req, "page"), "2") == 0);
    assert(req_get_query(&req, "missing") == NULL);

    /* A key with no '=' gets an empty-string value rather than being dropped */
    parse_query_string("flag&q=cats", &req);
    assert(req.query_count == 2);
    assert(strcmp(req_get_query(&req, "flag"), "") == 0);
    assert(strcmp(req_get_query(&req, "q"), "cats") == 0);

    /* Duplicate key: lookup returns the first occurrence */
    parse_query_string("q=first&q=second", &req);
    assert(req.query_count == 2);
    assert(strcmp(req_get_query(&req, "q"), "first") == 0);

    /* Consecutive '&'s don't produce a spurious empty pair (strtok_r skips
     * repeated delimiters, same as match_path does for repeated '/'). */
    parse_query_string("a=1&&b=2", &req);
    assert(req.query_count == 2);
    assert(strcmp(req_get_query(&req, "a"), "1") == 0);
    assert(strcmp(req_get_query(&req, "b"), "2") == 0);

    /* URL-decoding: '%XX' escapes and '+' both decode to their real bytes. */
    parse_query_string("name=a%20b&tag=c%2Bd&plus=a+b", &req);
    assert(strcmp(req_get_query(&req, "name"), "a b") == 0);
    assert(strcmp(req_get_query(&req, "tag"), "c+d") == 0);
    assert(strcmp(req_get_query(&req, "plus"), "a b") == 0);

    /* A malformed '%' escape (not followed by two hex digits) passes through
     * literally rather than being decoded or dropped. */
    parse_query_string("bad=100%25done&worse=a%2gb", &req);
    assert(strcmp(req_get_query(&req, "bad"), "100%done") == 0);
    assert(strcmp(req_get_query(&req, "worse"), "a%2gb") == 0);

    /* Percent-encoded key names are decoded too. */
    parse_query_string("a%20b=1", &req);
    assert(strcmp(req_get_query(&req, "a b"), "1") == 0);

    /* Overflow: pairs past MAX_QUERY_PARAMS are dropped, not overflowed. */
    char many[512] = "";
    for (int i = 0; i < MAX_QUERY_PARAMS + 5; i++) {
        char pair[16];
        snprintf(pair, sizeof(pair), "k%d=%d&", i, i);
        strncat(many, pair, sizeof(many) - strlen(many) - 1);
    }
    parse_query_string(many, &req);
    assert(req.query_count == MAX_QUERY_PARAMS);
}

int main(void) {
    test_extract_content_length();
    test_request_is_complete();
    test_request_wants_close();
    test_parse_http_request();
    test_request_has_chunked_encoding();
    test_chunked_body_scan();
    test_chunked_body_decode();
    test_request_is_complete_chunked();
    test_parse_http_request_chunked();
    test_status_text();
    test_url_decode();
    test_parse_headers();
    test_parse_cookies();
    test_parse_query_string();

    printf("all http parser tests passed\n");
    return 0;
}
