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
 *               oversized Content-Length, chunked + Content-Length together, bad chunk framing, a bare
 *               '\n' line ending anywhere in the request line or header block - S11, or a Transfer-
 *               Encoding this engine can't frame - S11, see req->content_length below)
 *           -2  request-target path >= sizeof(req->path) (256)  -> caller sends 414
 *           -3  retired (P3): used to mean "a header name/value would not fit its fixed-size copy" (S5),
 *               impossible now that req->headers holds views into `raw` instead of copies - any header
 *               that made it into a complete request already fits inside the whole header block, which
 *               was always capped independently (431 if the block itself exceeds BUF_SIZE, checked by
 *               the caller before this is ever reached). Kept reserved, not reused for something else,
 *               so old caller code branching on it is merely dead, never wrong. (This is this function's
 *               own top-level return code; req->content_length has an unrelated -3 of its own, below.)
 *           -4  a percent-decoded path, query name or query value contains an embedded NUL -> caller
 *               sends 400 (S6: "%00" - or a raw NUL byte - used to silently truncate everything read
 *               through it as a C string, e.g. "/style.css%00.png" looking like "/style.css" to a
 *               suffix check; never silently truncated now)
 *   On -1, req->content_length == -2 means "body too large"      -> caller sends 413.
 *   On -1, req->content_length == -3 means a Transfer-Encoding this engine can't frame (S11: anything
 *   other than exactly the single token "chunked" - a substring lookalike like "xchunked", "chunked"
 *   alongside any other coding regardless of order, or an unsupported coding alone, e.g. "gzip") ->
 *   caller sends 501 (the server understood the request but can't process that transfer-coding).
 *   Ownership: on success req->body is allocated from `arena` (content_length + 1 bytes, NUL-terminated,
 *   never NULL, may hold NUL bytes: use content_length, not strlen). Nobody free()s it: it is reclaimed
 *   when the arena is reset (the engine passes Connection.arena; tests pass their own Arena). It is NULL
 *   after any failure. `req->arena` is also set to this same arena (P3): req_get_header/req_get_cookie
 *   use it to materialize NUL-terminated strings out of the views/lazy split below. Only the fields
 *   behind *_count (and the scalars) are initialized; read arrays through the accessors.
 *   Decoding: req->path is percent-decoded; req->query stays raw; query names/values are
 *   percent- and '+'-decoded; header and cookie values are not decoded.
 *   Storage (P3): req->headers holds VIEWS into `raw` (name/value point into it, not NUL-terminated,
 *   valid only as long as `raw` is unchanged - i.e. until the handler returns, same rule as everything
 *   else reachable through req), not copies - there is no per-header size limit left to enforce, only
 *   the pre-existing whole-header-block cap (BUF_SIZE, checked by the caller). Cookies are still split
 *   into fixed-size copies (cookie_names/cookie_values), but lazily: parse_http_request does not call
 *   parse_cookies itself any more - req_get_cookie does, once, on its first call for this request.
 *   Limits (never overflowed): method 7 chars (longer -> -1); more than MAX_HEADERS headers -> -1 (the
 *   request is rejected, not truncated); query 255, MAX_QUERY_PARAMS and MAX_COOKIES (including each
 *   cookie's own name/value, 63/255 chars) are silently truncated or dropped. The request line and
 *   header block are parsed by the vendored picohttpparser (HTTP/1.x only); picohttpparser itself
 *   tolerates a bare '\n' line ending anywhere, but parse_request_head rejects any request that uses one
 *   (S11: closes a proxy-desync ambiguity a strict CRLF-only reader would not share).
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
 * chunk_scan (P8, may be NULL): resume state for a chunked body, carried across calls for the SAME
 * request as buf grows (zeroed before its first call, e.g. Connection.chunk_scan) - each body byte is
 * then scanned once in total instead of once per call. NULL scans from the body start every time.
 */
int request_head_is_complete(const ParsedHead *head, const char *buf, size_t len, ChunkScanState *chunk_scan);

/*
 * parse_http_request_from_head: the population half of parse_http_request, given an already-parsed
 * head (parse_request_head) instead of re-running phr_parse_request/request_framing on the same bytes
 * (P2). The caller must already have confirmed the request is ready to parse via request_head_is_complete
 * (true for both a genuinely complete head and a malformed request line, S8 - never call this while it
 * still returns false). head->header_len == 0 then means the request line itself was malformed (not
 * "incomplete" - that case never reaches here): returns -1 immediately, before touching head->method/path,
 * which are NULL in that case. Same return codes, ownership and limits as parse_http_request otherwise,
 * which is now a thin wrapper: parse_request_head + this.
 */
int parse_http_request_from_head(const char *raw, size_t raw_len, const ParsedHead *head, Request *req,
                                 Arena *arena);

/*
 * request_framing: locate the header block and decide how the body is framed.
 *   *header_len_out = bytes up to and including "\r\n\r\n", or 0 if headers are incomplete or malformed
 *     (a bare '\n' line ending anywhere, or a malformed request line - both S8/S11 - leave this at 0 too;
 *     told apart from "incomplete" only by the return value, see request_head_is_complete).
 *   *chunked_out    = 1 only when the Transfer-Encoding value's sole comma-separated token (OWS trimmed)
 *     is "chunked", case-insensitive (S11: not a substring search - "xchunked" or "chunked, gzip" don't
 *     count).
 *   *path_out / *path_len_out (both optional, pass NULL to skip) = the raw (not percent-decoded)
 *     request-target and its length, pointing into `buf`; valid only when the return value's
 *     caller can confirm *header_len_out > 0 (undefined content otherwise). Used by connection.c's
 *     S4 body-limit lookup to find a route/prefix before the body itself has to be parsed.
 *   Returns the Content-Length code: 0 absent/zero, >0 value, -1 malformed / conflicting
 *   duplicates / chunked+Content-Length, -2 above MAX_BODY_SIZE, -3 (S11) Transfer-Encoding names
 *   anything other than exactly the single token "chunked" -> caller sends 501.
 * Header names match exactly (case-insensitive) at line start: "X-Content-Length" never matches.
 */
int request_framing(const char *buf, size_t len, size_t *header_len_out, int *chunked_out,
                    const char **path_out, size_t *path_len_out);

/*
 * request_is_complete: 1 when buf[0..len) holds a full request (headers + framed body), when framing is
 * invalid/oversized, or when the request line/header block itself is malformed - "HTTP/2.0", garbage, no
 * HTTP version - in every one of those cases parse_http_request reports the specific error (S8).
 * 0 means "read more bytes" (the only remaining case). Called after every recv().
 */
int request_is_complete(const char *buf, size_t len);

/* Content-Length code (see request_framing) of a NUL-terminated header block or raw request. */
int extract_content_length(const char *header_block);

/* 1 when a Transfer-Encoding header of a NUL-terminated header block has "chunked" as its sole
 * comma-separated token (S11: token-exact, not a substring search - "xchunked" and "gzip, chunked" are 0). */
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
 * chunked_body_scan_resume (P8): same result codes, but continues from *state (zeroed = body start)
 * and advances it past every fully validated chunk, so calling it again after more bytes are
 * appended to the same body costs only the new bytes (plus one re-read size line). body_start and
 * the bytes already scanned must be unchanged between calls (offsets, so a realloc'd copy is fine).
 * chunked_body_scan is the from-scratch wrapper.
 */
int chunked_body_scan(const char *body_start, size_t available, size_t max_decoded_len, size_t *decoded_len_out);
int chunked_body_scan_resume(const char *body_start, size_t available, size_t max_decoded_len,
                             ChunkScanState *state, size_t *decoded_len_out);
size_t chunked_body_decode(const char *body_start, size_t available, char *out);

/* Component parsers (called by parse_http_request; exposed for tests). Each resets its own *_count.
 * parse_query_string returns 0 ok, -1 if a decoded name/value contains an embedded NUL (S6) - req is
 * still fully populated up to and including the offending pair, same "don't bother finishing what the
 * caller will reject anyway" convention as parse_http_request's own -4.
 * parse_headers (P3) fills req->headers with VIEWS into header_block, not copies - same storage
 * req_get_header expects, so it needs `arena` for the same reason parse_http_request does: materializing
 * a NUL-terminated value on a later req_get_header call. header_block must outlive any such call. */
int parse_query_string(const char *query, Request *req);    /* "a=1&b=2"; bare key -> "" */
void parse_headers(const char *header_block, Request *req, Arena *arena);  /* "Name: value\r\n..." */
void parse_cookies(const char *cookie_header, Request *req); /* NULL ok; "a=1; b=2" */

/* Accessors: first match, NULL when absent. Returned pointers live as long as `req`.
 * req_get_header (P3) materializes a NUL-terminated copy of the matching view into req->arena on every
 * call (not cached); returns NULL without allocating if req->arena is NULL (a Request no parse function
 * ever populated). req_get_cookie (P3) runs parse_cookies itself, once, the first time it's called for
 * a given req (req->cookies_parsed) - a request whose Cookie header nobody reads never pays for the
 * split at all. */
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
