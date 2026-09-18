#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

/*
 * Pure HTTP/1.1 request parsing: no I/O, no globals, `raw` is never modified.
 * Everything here is safe to call from a unit test with a string literal.
 */

#include <stddef.h>
#include "app_types.h"

/*
 * parse_http_request: fill `req` from a complete request in raw[0..raw_len).
 *   Returns  0  ok
 *           -1  malformed (bad request line, no header terminator, bad/duplicate/
 *               oversized Content-Length, chunked + Content-Length together, bad chunk framing)
 *           -2  request-target path >= sizeof(req->path) (256)  -> caller sends 414
 *   On -1, req->content_length == -2 means "body too large"      -> caller sends 413.
 *   Ownership: on success req->body is malloc'd (content_length + 1, NUL-terminated,
 *   never NULL, may hold NUL bytes: use content_length, not strlen). The CALLER free()s it
 *   on every path; it is NULL after any failure, so free(req.body) is always safe.
 *   Only the fields behind *_count are initialized; read arrays through the accessors.
 *   Decoding: req->path is percent-decoded; req->query stays raw; query names/values are
 *   percent- and '+'-decoded; header and cookie values are not decoded.
 *   Limits (excess is dropped or truncated, never overflowed): method 7 chars (longer -> -1),
 *   query 255, MAX_QUERY_PARAMS, MAX_HEADERS (name 63, value 255), MAX_COOKIES.
 */
int parse_http_request(const char *raw, size_t raw_len, Request *req);

/*
 * request_framing: locate the header block and decide how the body is framed.
 *   *header_len_out = bytes up to and including "\r\n\r\n", or 0 if headers are incomplete.
 *   *chunked_out    = 1 for "Transfer-Encoding: chunked".
 *   Returns the Content-Length code: 0 absent/zero, >0 value, -1 malformed / conflicting
 *   duplicates / chunked+Content-Length, -2 above MAX_BODY_SIZE.
 * Header names match exactly (case-insensitive) at line start: "X-Content-Length" never matches.
 */
int request_framing(const char *buf, size_t len, size_t *header_len_out, int *chunked_out);

/*
 * request_is_complete: 1 when buf[0..len) holds a full request (headers + framed body), or
 * when framing is invalid/oversized (stop buffering, parse_http_request reports it).
 * 0 means "read more bytes". Called after every recv().
 */
int request_is_complete(const char *buf, size_t len);

/* Content-Length code (see request_framing) of a NUL-terminated header block or raw request. */
int extract_content_length(const char *header_block);

/* 1 when a Transfer-Encoding header of a NUL-terminated header block lists "chunked". */
int request_has_chunked_encoding(const char *header_block);

/* 1 if the connection must close after this response: "Connection: close" wins, then
 * "Connection: keep-alive", else HTTP/1.1 keeps alive and everything else closes. */
int request_wants_close(const Request *req);

/*
 * Chunked request bodies (RFC 7230 4.1). chunked_body_scan validates framing without copying:
 *   1 complete (*decoded_len_out = decoded size), 0 need more bytes,
 *  -1 malformed framing, -2 decoded size would exceed max_decoded_len.
 * chunked_body_decode requires a prior scan result of 1 on the same input; `out` needs
 * decoded_len bytes; returns bytes written (binary-safe: never strlen the result).
 */
int chunked_body_scan(const char *body_start, size_t available, size_t max_decoded_len, size_t *decoded_len_out);
size_t chunked_body_decode(const char *body_start, size_t available, char *out);

/* Component parsers (called by parse_http_request; exposed for tests). Each resets its own *_count. */
void parse_query_string(const char *query, Request *req);   /* "a=1&b=2"; bare key -> "" */
void parse_headers(const char *header_block, Request *req);  /* "Name: value\r\n..." */
void parse_cookies(const char *cookie_header, Request *req); /* NULL ok; "a=1; b=2" */

/* Accessors: first match, NULL when absent. Returned pointers live as long as `req`. */
const char *req_get_query(const Request *req, const char *name);  /* case-sensitive, decoded */
const char *req_get_header(const Request *req, const char *name); /* case-INsensitive name */
const char *req_get_cookie(const Request *req, const char *name); /* case-sensitive, not decoded */

/* Percent-decode src into dst (dst_size incl. NUL, truncates). dst may equal src. '+' -> ' ' only if decode_plus.
 * A '%' without two hex digits is copied literally. */
void url_decode(const char *src, char *dst, size_t dst_size, int decode_plus);

/* Reason phrase for the 28 statuses the engine knows; "Unknown" otherwise. */
const char *status_text(int status);

#endif /* HTTP_PARSER_H */
