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
 *           -3  a header name (>= 64 chars) or value (>= MAX_HEADER_VALUE_LEN) would not fit -> caller
 *               sends 431 (S5: never silently truncated - a Bearer JWT or long cookie that overflowed
 *               the old 255-byte cap used to compare unequal to itself with no indication why)
 *           -4  a percent-decoded path, query name or query value contains an embedded NUL -> caller
 *               sends 400 (S6: "%00" - or a raw NUL byte - used to silently truncate everything read
 *               through it as a C string, e.g. "/style.css%00.png" looking like "/style.css" to a
 *               suffix check; never silently truncated now, same principle as -3 for headers)
 *   On -1, req->content_length == -2 means "body too large"      -> caller sends 413.
 *   Ownership: on success req->body is allocated from `arena` (content_length + 1 bytes, NUL-terminated,
 *   never NULL, may hold NUL bytes: use content_length, not strlen). Nobody free()s it: it is reclaimed
 *   when the arena is reset (the engine passes Connection.arena; tests pass their own Arena). It is NULL
 *   after any failure. Only the fields behind *_count are initialized; read arrays through the accessors.
 *   Decoding: req->path is percent-decoded; req->query stays raw; query names/values are
 *   percent- and '+'-decoded; header and cookie values are not decoded.
 *   Limits (never overflowed): method 7 chars (longer -> -1); more than MAX_HEADERS headers -> -1 (the
 *   request is rejected, not truncated); header name 63 chars / value MAX_HEADER_VALUE_LEN - 1 chars ->
 *   -3, not truncated (S5); query 255, MAX_QUERY_PARAMS and MAX_COOKIES (including each cookie's own
 *   name/value, 63/255 chars) are silently truncated or dropped. The request line and header block are
 *   parsed by the vendored picohttpparser (HTTP/1.x only; bare '\n' line endings are accepted).
 */
int parse_http_request(const char *raw, size_t raw_len, Request *req, Arena *arena);

/*
 * parse_request_head: runs phr_parse_request once over buf[0..len) and fills head with the raw
 * tokenization (method/path/headers) and the framing verdict (content-length/chunked, same codes
 * request_framing returns) so a caller that needs more than one of those - the body-limit check, the
 * completeness check, the full parse - doesn't pay for its own pass over the same bytes (P2).
 * head->header_len == 0 means incomplete or malformed (distinguished only by this function's return
 * value, exactly as request_framing already worked): every other field is then not meaningful.
 * request_framing and request_is_complete are now thin wrappers over this plus request_head_is_complete;
 * they keep their own signatures for existing callers and are unaffected in behavior.
 */
int parse_request_head(const char *buf, size_t len, ParsedHead *head);

/*
 * request_head_is_complete: the request_is_complete logic against an already-parsed head instead of
 * re-parsing (P2). head must come from parse_request_head over buf[0..len). Same return convention as
 * request_is_complete.
 */
int request_head_is_complete(const ParsedHead *head, const char *buf, size_t len);

/*
 * parse_http_request_from_head: the population half of parse_http_request, given an already-parsed
 * head (parse_request_head) instead of re-running phr_parse_request/request_framing on the same bytes
 * (P2). head->header_len must be > 0 (the caller already confirmed headers are complete, e.g. via
 * request_head_is_complete). Same return codes, ownership and limits as parse_http_request, which is
 * now a thin wrapper: parse_request_head + this.
 */
int parse_http_request_from_head(const char *raw, size_t raw_len, const ParsedHead *head, Request *req,
                                 Arena *arena);

/*
 * request_framing: locate the header block and decide how the body is framed.
 *   *header_len_out = bytes up to and including "\r\n\r\n", or 0 if headers are incomplete.
 *   *chunked_out    = 1 for "Transfer-Encoding: chunked".
 *   *path_out / *path_len_out (both optional, pass NULL to skip) = the raw (not percent-decoded)
 *     request-target and its length, pointing into `buf`; valid only when the return value's
 *     caller can confirm *header_len_out > 0 (undefined content otherwise). Used by connection.c's
 *     S4 body-limit lookup to find a route/prefix before the body itself has to be parsed.
 *   Returns the Content-Length code: 0 absent/zero, >0 value, -1 malformed / conflicting
 *   duplicates / chunked+Content-Length, -2 above MAX_BODY_SIZE.
 * Header names match exactly (case-insensitive) at line start: "X-Content-Length" never matches.
 */
int request_framing(const char *buf, size_t len, size_t *header_len_out, int *chunked_out,
                    const char **path_out, size_t *path_len_out);

/*
 * request_is_complete: 1 when buf[0..len) holds a full request (headers + framed body), or
 * when framing is invalid/oversized (stop buffering, parse_http_request reports it).
 * 0 means "read more bytes". Called after every recv().
 * Known gap: a request line or header block that picohttpparser rejects (no HTTP version, "HTTP/2.0", garbage)
 * also returns 0, so the connection waits for more bytes instead of getting a 400 (lib/CLAUDE.md, "Known gaps").
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

/* Component parsers (called by parse_http_request; exposed for tests). Each resets its own *_count.
 * parse_query_string returns 0 ok, -1 if a decoded name/value contains an embedded NUL (S6) - req is
 * still fully populated up to and including the offending pair, same "don't bother finishing what the
 * caller will reject anyway" convention as parse_http_request's own -3. */
int parse_query_string(const char *query, Request *req);    /* "a=1&b=2"; bare key -> "" */
void parse_headers(const char *header_block, Request *req);  /* "Name: value\r\n..." */
void parse_cookies(const char *cookie_header, Request *req); /* NULL ok; "a=1; b=2" */

/* Accessors: first match, NULL when absent. Returned pointers live as long as `req`. */
const char *req_get_query(const Request *req, const char *name);  /* case-sensitive, decoded */
const char *req_get_header(const Request *req, const char *name); /* case-INsensitive name */
const char *req_get_cookie(const Request *req, const char *name); /* case-sensitive, not decoded */

/* Percent-decode src into dst (dst_size incl. NUL, truncates). dst may equal src. '+' -> ' ' only if decode_plus.
 * A '%' without two hex digits is copied literally. Returns 0 ok, -1 if a decoded byte is NUL (S6) -
 * dst is still fully written and NUL-terminated, but the caller should treat it as invalid input. */
int url_decode(const char *src, char *dst, size_t dst_size, int decode_plus);

/* Reason phrase for the 28 statuses the engine knows; "Unknown" otherwise. */
const char *status_text(int status);

#endif /* HTTP_PARSER_H */
