#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_types.h"
#include "http_parser.h"
#include "arena.h"

Arena test_arena;
char test_arena_buf[64 * 1024];

static void test_extract_content_length(void) {
    /* Absent Content-Length returns 0 */
    assert(extract_content_length("Host: localhost\r\nAccept: */*\r\n\r\n") == 0);

    /* Standard valid Content-Length */
    assert(extract_content_length("Content-Length: 42\r\n\r\n") == 42);
    assert(extract_content_length("Host: localhost\r\nContent-Length: 0\r\n") == 0);

    /* Case-insensitivity */
    assert(extract_content_length("content-length: 123\r\n") == 123);
    assert(extract_content_length("CONTENT-LENGTH: 99\r\n") == 99);

    /* Negative value: malformed, sentinel -1 (-> 400 Bad Request) */
    assert(extract_content_length("Content-Length: -1\r\n\r\n") == -1);
    assert(extract_content_length("Content-Length: -100\r\n") == -1);

    /* A body well beyond the old BUF_SIZE is fine now - body storage is
     * decoupled from BUF_SIZE (see lib/CLAUDE.md, "Behavior reference, Buffers"). */
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
    parse_headers("Host: localhost\r\nConnection: close\r\n", &req, &test_arena);
    assert(request_wants_close(&req) == 1);

    /* Explicit Connection: keep-alive overrides HTTP/1.0 */
    memset(&req, 0, sizeof(req));
    strncpy(req.version, "HTTP/1.0", sizeof(req.version) - 1);
    parse_headers("Host: localhost\r\nConnection: keep-alive\r\n", &req, &test_arena);
    assert(request_wants_close(&req) == 0);

    /* HTTP/1.1 default without Connection header is keep-alive */
    memset(&req, 0, sizeof(req));
    strncpy(req.version, "HTTP/1.1", sizeof(req.version) - 1);
    parse_headers("Host: localhost\r\n", &req, &test_arena);
    assert(request_wants_close(&req) == 0);

    /* HTTP/1.0 default without Connection header is close */
    memset(&req, 0, sizeof(req));
    strncpy(req.version, "HTTP/1.0", sizeof(req.version) - 1);
    parse_headers("Host: localhost\r\n", &req, &test_arena);
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
    assert(parse_http_request(raw_get, strlen(raw_get), &req, &test_arena) == 0);
    assert(strcmp(req.method, "GET") == 0);
    assert(strcmp(req.path, "/items") == 0);
    assert(strcmp(req.query, "") == 0);
    assert(strcmp(req.version, "HTTP/1.1") == 0);
    assert(req.content_length == 0);
    assert(req.body != NULL);
    assert(strcmp(req.body, "") == 0);
    assert(strcmp(req_get_header(&req, "Host"), "example.com") == 0);
    arena_reset(&test_arena);

    /* Request line with zero header lines before the blank-line terminator
     * (request-line CRLF immediately followed by the blank-line CRLF).
     * Regression test: this used to make the request-line's own "\r\n"
     * coincide with the start of "\r\n\r\n", producing a negative
     * header_start-to-header_end distance that wrapped to a huge size_t and
     * caused an out-of-bounds heap read while copying the header block
     * (found via ASan) - a header-less request is unusual but perfectly
     * legal to receive and must not crash/over-read. */
    const char *raw_no_headers = "GET /ping HTTP/1.1\r\n\r\n";
    assert(parse_http_request(raw_no_headers, strlen(raw_no_headers), &req, &test_arena) == 0);
    assert(strcmp(req.path, "/ping") == 0);
    assert(req.header_count == 0);
    arena_reset(&test_arena);

    /* GET with query string */
    const char *raw_query = "GET /search?q=test&page=2 HTTP/1.1\r\nHost: example.com\r\n\r\n";
    assert(parse_http_request(raw_query, strlen(raw_query), &req, &test_arena) == 0);
    assert(strcmp(req.method, "GET") == 0);
    assert(strcmp(req.path, "/search") == 0);
    assert(strcmp(req.query, "q=test&page=2") == 0);
    /* parse_http_request also populates the parsed query-param arrays. */
    assert(strcmp(req_get_query(&req, "q"), "test") == 0);
    assert(strcmp(req_get_query(&req, "page"), "2") == 0);
    arena_reset(&test_arena);

    /* Percent-encoded path segment is URL-decoded; req->query stays raw
     * (unparsed), only the parsed query-param arrays are decoded. */
    const char *raw_encoded = "GET /a%20b/caf%C3%A9?name=a%20b HTTP/1.1\r\nHost: example.com\r\n\r\n";
    assert(parse_http_request(raw_encoded, strlen(raw_encoded), &req, &test_arena) == 0);
    assert(strcmp(req.path, "/a b/caf\xC3\xA9") == 0);
    assert(strcmp(req.query, "name=a%20b") == 0);
    assert(strcmp(req_get_query(&req, "name"), "a b") == 0);
    arena_reset(&test_arena);

    /* POST with body and Content-Length */
    const char *raw_post =
        "POST /echo HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 13\r\n"
        "\r\n"
        "Hello, World!";
    assert(parse_http_request(raw_post, strlen(raw_post), &req, &test_arena) == 0);
    assert(strcmp(req.method, "POST") == 0);
    assert(strcmp(req.path, "/echo") == 0);
    assert(req.content_length == 13);
    assert(strcmp(req.body, "Hello, World!") == 0);
    /* parse_http_request also populates the parsed header map. */
    assert(strcmp(req_get_header(&req, "Host"), "localhost") == 0);
    assert(strcmp(req_get_header(&req, "content-length"), "13") == 0);
    assert(req_get_header(&req, "X-Missing") == NULL);
    arena_reset(&test_arena);

    /* parse_http_request also populates the parsed cookie map, off the
     * "Cookie" header (case-insensitive lookup finds it via req_get_header,
     * cookie name lookup itself stays case-sensitive per RFC 6265). */
    const char *raw_with_cookies =
        "GET /profile HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Cookie: session=abc123; theme=dark\r\n"
        "\r\n";
    assert(parse_http_request(raw_with_cookies, strlen(raw_with_cookies), &req, &test_arena) == 0);
    assert(strcmp(req_get_cookie(&req, "session"), "abc123") == 0);
    assert(strcmp(req_get_cookie(&req, "theme"), "dark") == 0);
    assert(req_get_cookie(&req, "missing") == NULL);
    arena_reset(&test_arena);

    /* No Cookie header at all -> zero parsed cookies, not a parse error. Cookie splitting is lazy
     *: req.cookie_count reads 0 immediately after parsing regardless (nothing has run yet), so
     * force the lazy parse via req_get_cookie first to actually exercise the "absent Cookie header"
     * path through parse_cookies, not just an unparsed default. */
    const char *raw_no_cookies = "GET /profile HTTP/1.1\r\nHost: localhost\r\n\r\n";
    assert(parse_http_request(raw_no_cookies, strlen(raw_no_cookies), &req, &test_arena) == 0);
    assert(req_get_cookie(&req, "session") == NULL);
    assert(req.cookie_count == 0);
    arena_reset(&test_arena);

    /* A body well beyond the old BUF_SIZE (8192) now parses cleanly -
     * req->body is heap-allocated to fit it exactly rather than being
     * copied into a BUF_SIZE-capped fixed array (see lib/CLAUDE.md,
     * "Behavior reference, Buffers"). */
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
    assert(parse_http_request(big_raw, big_head_len + big_len, &req, &test_arena) == 0);
    assert(req.content_length == (int)big_len);
    assert(strlen(req.body) == big_len);
    assert(strcmp(req.body, big_body) == 0);
    arena_reset(&test_arena);
    free(big_raw);
    free(big_body);

    /* Malformed request line */
    const char *malformed_line = "INVALID\r\n\r\n";
    assert(parse_http_request(malformed_line, strlen(malformed_line), &req, &test_arena) == -1);

    /* Missing header terminator */
    const char *no_term = "GET / HTTP/1.1\r\nHost: example.com";
    assert(parse_http_request(no_term, strlen(no_term), &req, &test_arena) == -1);

    /* Negative Content-Length */
    const char *bad_cl = "POST / HTTP/1.1\r\nContent-Length: -5\r\n\r\nbody";
    assert(parse_http_request(bad_cl, strlen(bad_cl), &req, &test_arena) == -1);

    /* Content-Length exceeding MAX_BODY_SIZE is still rejected, just at a
     * much higher ceiling than the old BUF_SIZE-based one. */
    char over_head[64];
    snprintf(over_head, sizeof(over_head), "POST / HTTP/1.1\r\nContent-Length: %d\r\n\r\nbody", MAX_BODY_SIZE + 1);
    assert(parse_http_request(over_head, strlen(over_head), &req, &test_arena) == -1);

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
    assert(parse_http_request(long_raw, strlen(long_raw), &req, &test_arena) == -2);
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
    assert(parse_http_request(fit_raw, strlen(fit_raw), &req, &test_arena) == 0);
    assert(strlen(req.path) == fit_path_len);
    arena_reset(&test_arena);
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
    assert(parse_http_request(binary_raw, binary_head_len + sizeof(binary_body), &req, &test_arena) == 0);
    assert(req.content_length == (int)sizeof(binary_body));
    assert(memcmp(req.body, binary_body, sizeof(binary_body)) == 0);
    arena_reset(&test_arena);
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

/* feeding a body one byte at a time through chunked_body_scan_resume (one state, carried across
 * calls, like Connection.chunk_scan across recvs) must give the same verdict and decoded length as a
 * from-scratch chunked_body_scan of the same prefix, for every prefix - including a trailer's
 * terminating "\r\n\r\n" split across calls, malformed framing, and the size cap. */
static void assert_resume_matches_scratch(const char *body, const size_t max_decoded_len) {
    const size_t len = strlen(body);
    ChunkScanState state = {0};
    for (size_t avail = 0; avail <= len; avail++) {
        size_t scratch_len = 0;
        size_t resume_len = 0;
        const int scratch = chunked_body_scan(body, avail, max_decoded_len, &scratch_len);
        const int resumed = chunked_body_scan_resume(body, avail, max_decoded_len, &state, &resume_len);
        assert(scratch == resumed);
        assert(scratch_len == resume_len);
        assert(state.pos <= avail);
        if (resumed != 0) {
            return; /* a caller stops reading at the first non-zero verdict */
        }
    }
}

static void test_chunked_body_scan_resume(void) {
    assert_resume_matches_scratch("4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n", MAX_BODY_SIZE);
    assert_resume_matches_scratch("4;ext=1\r\nWiki\r\n0\r\nExpires: never\r\nX-A: b\r\n\r\n", MAX_BODY_SIZE);
    assert_resume_matches_scratch("1\r\na\r\n1\r\nb\r\n1\r\nc\r\n0\r\n\r\n", MAX_BODY_SIZE);
    assert_resume_matches_scratch("4\r\nWikiXX0\r\n\r\n", MAX_BODY_SIZE);       /* bad data CRLF */
    assert_resume_matches_scratch("4\r\nWiki\r\nZZ\r\nWiki\r\n0\r\n\r\n", MAX_BODY_SIZE); /* bad hex later */
    assert_resume_matches_scratch("4\r\nWiki\r\n4\r\npedi\r\n0\r\n\r\n", 6);        /* cumulative cap */

    /* The state only moves past fully validated chunks: after the first chunk and half of the
     * second, pos sits at the second chunk's size line and decoded_len counts the first alone. */
    const char *body = "4\r\nWiki\r\n5\r\nped";
    ChunkScanState state = {0};
    size_t decoded_len = 0;
    assert(chunked_body_scan_resume(body, strlen(body), MAX_BODY_SIZE, &state, &decoded_len) == 0);
    assert(state.pos == 9);
    assert(state.decoded_len == 4);
    assert(decoded_len == 4);

    /* Resuming from a state that is already past a chunk does not re-read it: poisoning the bytes
     * before state.pos changes nothing (proves the scan really starts at state.pos). */
    char poisoned[] = "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
    ChunkScanState mid = {0};
    assert(chunked_body_scan_resume(poisoned, 12, MAX_BODY_SIZE, &mid, &decoded_len) == 0);
    assert(mid.pos == 9);
    memset(poisoned, 'Z', mid.pos);
    assert(chunked_body_scan_resume(poisoned, strlen(poisoned), MAX_BODY_SIZE, &mid, &decoded_len) == 1);
    assert(decoded_len == 9);

    /* Trailer search resumes too: a long trailer dripped in pieces moves trailer_from forward. */
    const char *trailer = "0\r\nX-Long: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n\r\n";
    ChunkScanState t = {0};
    assert(chunked_body_scan_resume(trailer, 20, MAX_BODY_SIZE, &t, &decoded_len) == 0);
    assert(t.pos == 0);
    assert(t.trailer_from == 17);
    assert(chunked_body_scan_resume(trailer, strlen(trailer), MAX_BODY_SIZE, &t, &decoded_len) == 1);
    assert(decoded_len == 0);
}

/* request_wire_len is where a pipelined next request starts - headers plus the framed body,
 * never anything after it. */
static void test_request_wire_len(void) {
    const char *next = "GET /next HTTP/1.1\r\n\r\n";
    char buf[512];

    const char *get = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    snprintf(buf, sizeof(buf), "%s%s", get, next);
    ParsedHead head;
    ChunkScanState scan = {0};
    parse_request_head(buf, strlen(buf), &head);
    assert(request_head_is_complete(&head, buf, strlen(buf), &scan) == 1);
    assert(request_wire_len(&head, &scan) == strlen(get));

    const char *post = "POST / HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello";
    snprintf(buf, sizeof(buf), "%s%s", post, next);
    parse_request_head(buf, strlen(buf), &head);
    assert(request_head_is_complete(&head, buf, strlen(buf), &scan) == 1);
    assert(request_wire_len(&head, &scan) == strlen(post));

    const char *chunked = "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
                          "4\r\nWiki\r\n0\r\nX-T: v\r\n\r\n";
    snprintf(buf, sizeof(buf), "%s%s", chunked, next);
    scan = (ChunkScanState){0};
    parse_request_head(buf, strlen(buf), &head);
    assert(request_head_is_complete(&head, buf, strlen(buf), &scan) == 1);
    assert(request_wire_len(&head, &scan) == strlen(chunked));
    assert(strncmp(buf + request_wire_len(&head, &scan), "GET /next", 9) == 0);
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
    assert(parse_http_request(raw, strlen(raw), &req, &test_arena) == 0);
    assert(strcmp(req.method, "POST") == 0);
    assert(strcmp(req.path, "/echo") == 0);
    assert(req.content_length == 9);
    assert(strcmp(req.body, "Wikipedia") == 0);
    arena_reset(&test_arena);

    /* Trailer headers are accepted but not surfaced anywhere on Request. */
    const char *with_trailer = "POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
                                "4\r\nWiki\r\n0\r\nExpires: never\r\n\r\n";
    assert(parse_http_request(with_trailer, strlen(with_trailer), &req, &test_arena) == 0);
    assert(strcmp(req.body, "Wiki") == 0);
    arena_reset(&test_arena);

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
    assert(parse_http_request(binary_raw, head_len + body_len, &req, &test_arena) == 0);
    assert(req.content_length == 3);
    assert(memcmp(req.body, "a\0b", 3) == 0);
    arena_reset(&test_arena);

    /* RFC 7230 3.3.3: Transfer-Encoding and Content-Length together is an
     * ambiguous/smuggling-shaped message - rejected outright (400), not
     * resolved by preferring one header over the other. */
    const char *both_headers = "POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n"
                                "Content-Length: 4\r\n\r\n4\r\nWiki\r\n0\r\n\r\n";
    assert(parse_http_request(both_headers, strlen(both_headers), &req, &test_arena) == -1);
    assert(req.body == NULL);

    /* Malformed chunk framing fails the parse (-1), same status family a
     * malformed Content-Length gets. */
    const char *malformed = "POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\nZZ\r\nWiki\r\n0\r\n\r\n";
    assert(parse_http_request(malformed, strlen(malformed), &req, &test_arena) == -1);
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
    parse_headers("", &req, &test_arena);
    assert(req.header_count == 0);
    assert(req_get_header(&req, "Host") == NULL);

    /* Basic Name: value pairs, leading space after ':' trimmed */
    parse_headers("Host: example.com\r\nContent-Type: application/json\r\n", &req, &test_arena);
    assert(req.header_count == 2);
    assert(strcmp(req_get_header(&req, "Host"), "example.com") == 0);
    assert(strcmp(req_get_header(&req, "Content-Type"), "application/json") == 0);

    /* Lookup is case-insensitive on the header name (RFC 7230) */
    assert(strcmp(req_get_header(&req, "host"), "example.com") == 0);
    assert(strcmp(req_get_header(&req, "CONTENT-TYPE"), "application/json") == 0);

    /* A value with no leading space after ':' is still captured as-is */
    parse_headers("X-Flag:on\r\n", &req, &test_arena);
    assert(strcmp(req_get_header(&req, "X-Flag"), "on") == 0);
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

/* parse_http_request_in_place gives the same Request as the copying parser, but req->body points
 * into the caller's buffer (no copy); a chunked body is decoded over its own framing. The byte its
 * NUL replaced is handed back, and restoring it leaves the bytes after the body unchanged. */
static void check_in_place_matches_copy(const char *wire, const size_t request_len) {
    const size_t wire_len = strlen(wire);
    char *buf = malloc(wire_len + 1);
    assert(buf != NULL);
    memcpy(buf, wire, wire_len + 1);

    Request copied;
    arena_reset(&test_arena);
    assert(parse_http_request(wire, request_len, &copied, &test_arena) == 0);

    ParsedHead head;
    parse_request_head(buf, request_len, &head);
    Request in_place;
    char saved = 'x';
    assert(parse_http_request_in_place(buf, request_len, &head, &in_place, &test_arena, &saved) == 0);

    assert(in_place.body == buf + head.header_len); /* a view, not an arena copy */
    assert(in_place.content_length == copied.content_length);
    assert(memcmp(in_place.body, copied.body, (size_t)copied.content_length) == 0);
    assert(in_place.body[in_place.content_length] == '\0');
    assert(strcmp(in_place.method, copied.method) == 0 && strcmp(in_place.path, copied.path) == 0);
    assert(in_place.header_count == copied.header_count);

    in_place.body[in_place.content_length] = saved;
    /* Everything after the request (a pipelined next one, or the terminating NUL) is intact. */
    assert(memcmp(buf + request_len, wire + request_len, wire_len - request_len + 1) == 0);
    if (!head.chunked) {
        assert(memcmp(buf, wire, wire_len + 1) == 0); /* Content-Length: nothing modified at all */
    }
    free(buf);
}

static void test_parse_http_request_in_place(void) {
    /* Content-Length body followed by a pipelined request: the NUL lands on its 'G' and is restored. */
    const char *pipelined = "POST /u HTTP/1.1\r\nContent-Length: 5\r\n\r\nhelloGET /next HTTP/1.1\r\n\r\n";
    check_in_place_matches_copy(pipelined, strstr(pipelined, "GET /next") - pipelined);

    /* Body ends the buffer: the NUL goes on raw[raw_len], already '\0'. */
    const char *at_end = "POST /u HTTP/1.1\r\nContent-Length: 3\r\n\r\nabc";
    check_in_place_matches_copy(at_end, strlen(at_end));

    /* No body: an empty, NUL-terminated view (used to be a 1-byte arena allocation). */
    const char *no_body = "GET /a HTTP/1.1\r\nHost: x\r\n\r\nGET /b HTTP/1.1\r\n\r\n";
    check_in_place_matches_copy(no_body, strstr(no_body, "GET /b") - no_body);

    /* Chunked with an extension, a trailer and a pipelined request behind it: decoded in place. */
    const char *chunked = "POST /u HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
                          "4;ext=1\r\nWiki\r\n5\r\npedia\r\nE\r\n in\r\n\r\nchunks.\r\n0\r\nX-T: 1\r\n\r\n"
                          "GET /next HTTP/1.1\r\n\r\n";
    check_in_place_matches_copy(chunked, strstr(chunked, "GET /next") - chunked);

    /* Binary chunk data (embedded NULs), data longer than its own size line so source and destination
     * overlap during the in-place move. */
    char binary[512];
    int n = snprintf(binary, sizeof(binary), "POST /u HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n40\r\n");
    for (int i = 0; i < 0x40; i++) binary[n++] = (char)(i % 7 == 0 ? 0 : 'a' + i % 26);
    n += snprintf(binary + n, sizeof(binary) - (size_t)n, "\r\n0\r\n\r\n");
    char *bin_buf = malloc((size_t)n + 1);
    assert(bin_buf != NULL);
    memcpy(bin_buf, binary, (size_t)n);
    bin_buf[n] = '\0';
    Request copied;
    arena_reset(&test_arena);
    assert(parse_http_request(binary, (size_t)n, &copied, &test_arena) == 0);
    assert(copied.content_length == 0x40);
    ParsedHead head;
    parse_request_head(bin_buf, (size_t)n, &head);
    Request in_place;
    char saved;
    assert(parse_http_request_in_place(bin_buf, (size_t)n, &head, &in_place, &test_arena, &saved) == 0);
    assert(in_place.content_length == 0x40 && memcmp(in_place.body, copied.body, 0x40) == 0);
    free(bin_buf);

    /* A failed parse writes nothing: a malformed chunk leaves the buffer byte-for-byte unchanged. */
    const char *bad = "POST /u HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nabc\r\n0\r\n\r\n";
    char bad_buf[128];
    memcpy(bad_buf, bad, strlen(bad) + 1);
    parse_request_head(bad_buf, strlen(bad), &head);
    saved = 'q';
    assert(parse_http_request_in_place(bad_buf, strlen(bad), &head, &in_place, &test_arena, &saved) == -1);
    assert(strcmp(bad_buf, bad) == 0 && saved == 'q');
}

/* which heads ask for "100 Continue". Pure over a buffer: parse_request_head then the predicate. */
static int expects_continue(const char *raw) {
    ParsedHead head;
    parse_request_head(raw, strlen(raw), &head);
    return request_head_expects_continue(&head);
}

static void test_request_head_expects_continue(void) {
    assert(expects_continue("POST /u HTTP/1.1\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\n") == 1);
    /* Name and value are case-insensitive; surrounding whitespace is not part of the value. */
    assert(expects_continue("POST /u HTTP/1.1\r\nexpect:   100-Continue  \r\nContent-Length: 5\r\n\r\n") == 1);
    /* Chunked has a body to wait for, even with no Content-Length. */
    assert(expects_continue("POST /u HTTP/1.1\r\nTransfer-Encoding: chunked\r\nExpect: 100-continue\r\n\r\n") == 1);
    /* HTTP/1.0: a 1xx must never be sent. */
    assert(expects_continue("POST /u HTTP/1.0\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\n") == 0);
    /* No body to wait for. */
    assert(expects_continue("POST /u HTTP/1.1\r\nExpect: 100-continue\r\n\r\n") == 0);
    assert(expects_continue("POST /u HTTP/1.1\r\nContent-Length: 0\r\nExpect: 100-continue\r\n\r\n") == 0);
    /* No Expect, another expectation, or a value that merely contains the token. */
    assert(expects_continue("POST /u HTTP/1.1\r\nContent-Length: 5\r\n\r\n") == 0);
    assert(expects_continue("POST /u HTTP/1.1\r\nContent-Length: 5\r\nExpect: something-else\r\n\r\n") == 0);
    assert(expects_continue("POST /u HTTP/1.1\r\nContent-Length: 5\r\nExpect: 100-continuex\r\n\r\n") == 0);
    assert(expects_continue("POST /u HTTP/1.1\r\nContent-Length: 5\r\nX-Expect: 100-continue\r\n\r\n") == 0);
    /* Invalid framing is answered (400) without inviting a body. */
    assert(expects_continue("POST /u HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 6\r\n"
                            "Expect: 100-continue\r\n\r\n") == 0);
    /* Incomplete head: nothing known yet. */
    assert(expects_continue("POST /u HTTP/1.1\r\nContent-Length: 5\r\nExpect: 100-continue\r\n") == 0);
}

/* IMF-fixdate formatting, against reference values from Python's email.utils.formatdate(usegmt=True). */
static void test_format_http_date(void) {
    char out[HTTP_DATE_LEN + 1];
    memset(out, 'x', sizeof(out));
    format_http_date(0, out);
    assert(strcmp(out, "Thu, 01 Jan 1970 00:00:00 GMT") == 0);
    assert(strlen(out) == HTTP_DATE_LEN);
    format_http_date(784111777, out);
    assert(strcmp(out, "Sun, 06 Nov 1994 08:49:37 GMT") == 0); /* RFC 9110's own example */
    format_http_date(951782400, out);
    assert(strcmp(out, "Tue, 29 Feb 2000 00:00:00 GMT") == 0); /* leap day of a /400 year */
    format_http_date(951868799, out);
    assert(strcmp(out, "Tue, 29 Feb 2000 23:59:59 GMT") == 0);
    format_http_date((time_t)4102444800LL, out);
    assert(strcmp(out, "Fri, 01 Jan 2100 00:00:00 GMT") == 0); /* 2100 is not a leap year */
    format_http_date(1790170496, out);
    assert(strcmp(out, "Wed, 23 Sep 2026 13:34:56 GMT") == 0);
    format_http_date((time_t)253402300799LL, out);
    assert(strcmp(out, "Fri, 31 Dec 9999 23:59:59 GMT") == 0); /* last four-digit-year second */
    format_http_date(-5, out);
    assert(strcmp(out, "Thu, 01 Jan 1970 00:00:00 GMT") == 0); /* pre-epoch clamps */
}

/* the cache hands back one stable buffer, reformatted only when the second changes. */
static void test_http_date_for_caches_per_second(void) {
    const char *a = http_date_for(784111777);
    assert(strcmp(a, "Sun, 06 Nov 1994 08:49:37 GMT") == 0);
    const char *b = http_date_for(784111777);
    assert(a == b && strcmp(b, "Sun, 06 Nov 1994 08:49:37 GMT") == 0);
    const char *c = http_date_for(784111778);
    assert(c == a && strcmp(c, "Sun, 06 Nov 1994 08:49:38 GMT") == 0);
}

int main(void) {
    test_format_http_date();
    test_http_date_for_caches_per_second();
    arena_init(&test_arena, test_arena_buf, sizeof(test_arena_buf));
    test_extract_content_length();
    test_request_is_complete();
    test_request_wants_close();
    test_request_head_expects_continue();
    test_parse_http_request();
    test_request_has_chunked_encoding();
    test_chunked_body_scan();
    test_chunked_body_scan_resume();
    test_request_wire_len();
    test_chunked_body_decode();
    test_request_is_complete_chunked();
    test_parse_http_request_chunked();
    test_parse_http_request_in_place();
    test_status_text();
    test_url_decode();
    test_parse_headers();
    test_parse_cookies();
    test_parse_query_string();

    printf("all http parser tests passed\n");
    return 0;
}
