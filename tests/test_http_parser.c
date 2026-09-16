#include <assert.h>
#include <stdio.h>
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

    /* Negative value rejected */
    assert(extract_content_length("Content-Length: -1\r\n") == -1);
    assert(extract_content_length("Content-Length: -100\r\n") == -1);

    /* Exceeding BUF_SIZE - 1 rejected */
    assert(extract_content_length("Content-Length: 8192\r\n") == -1);
    assert(extract_content_length("Content-Length: 999999\r\n") == -1);

    /* Boundary: exact max allowed BUF_SIZE - 1 */
    assert(extract_content_length("Content-Length: 8191\r\n") == BUF_SIZE - 1);
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
    assert(parse_http_request(raw_get, &req) == 0);
    assert(strcmp(req.method, "GET") == 0);
    assert(strcmp(req.path, "/items") == 0);
    assert(strcmp(req.query, "") == 0);
    assert(strcmp(req.version, "HTTP/1.1") == 0);
    assert(req.content_length == 0);
    assert(strcmp(req.body, "") == 0);
    assert(strstr(req.headers, "Host: example.com") != NULL);

    /* GET with query string */
    const char *raw_query = "GET /search?q=test&page=2 HTTP/1.1\r\nHost: example.com\r\n\r\n";
    assert(parse_http_request(raw_query, &req) == 0);
    assert(strcmp(req.method, "GET") == 0);
    assert(strcmp(req.path, "/search") == 0);
    assert(strcmp(req.query, "q=test&page=2") == 0);

    /* POST with body and Content-Length */
    const char *raw_post =
        "POST /echo HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 13\r\n"
        "\r\n"
        "Hello, World!";
    assert(parse_http_request(raw_post, &req) == 0);
    assert(strcmp(req.method, "POST") == 0);
    assert(strcmp(req.path, "/echo") == 0);
    assert(req.content_length == 13);
    assert(strcmp(req.body, "Hello, World!") == 0);

    /* Malformed request line */
    const char *malformed_line = "INVALID\r\n\r\n";
    assert(parse_http_request(malformed_line, &req) == -1);

    /* Missing header terminator */
    const char *no_term = "GET / HTTP/1.1\r\nHost: example.com";
    assert(parse_http_request(no_term, &req) == -1);

    /* Negative Content-Length */
    const char *bad_cl = "POST / HTTP/1.1\r\nContent-Length: -5\r\n\r\nbody";
    assert(parse_http_request(bad_cl, &req) == -1);

    /* Content-Length exceeding buffer */
    const char *huge_cl = "POST / HTTP/1.1\r\nContent-Length: 90000\r\n\r\nbody";
    assert(parse_http_request(huge_cl, &req) == -1);
}

static void test_status_text(void) {
    assert(strcmp(status_text(200), "OK") == 0);
    assert(strcmp(status_text(201), "Created") == 0);
    assert(strcmp(status_text(204), "No Content") == 0);
    assert(strcmp(status_text(400), "Bad Request") == 0);
    assert(strcmp(status_text(401), "Unauthorized") == 0);
    assert(strcmp(status_text(403), "Forbidden") == 0);
    assert(strcmp(status_text(404), "Not Found") == 0);
    assert(strcmp(status_text(431), "Request Header Fields Too Large") == 0);
    assert(strcmp(status_text(500), "Internal Server Error") == 0);
    assert(strcmp(status_text(418), "Unknown") == 0);
    assert(strcmp(status_text(0), "Unknown") == 0);
}

int main(void) {
    test_extract_content_length();
    test_request_is_complete();
    test_request_wants_close();
    test_parse_http_request();
    test_status_text();

    printf("all http parser tests passed\n");
    return 0;
}
