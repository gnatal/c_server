#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

/*
 * Pure HTTP/1.1 request parsing: no I/O, no globals, `raw` is never modified.
 * Everything here is safe to call from a unit test with a string literal.
 */

#include <stddef.h>
#include <time.h>
#include "app_types.h"

/*
 * parse_http_request: fill `req` from a complete request in raw[0..raw_len).
 *   Returns  0  ok
 *           -1  malformed (bad request line, no header terminator, bad/duplicate/
 *               oversized Content-Length, chunked + Content-Length together, bad chunk framing, a bare
 *               '\n' line ending anywhere in the request line or header block, or a Transfer-
 *               Encoding this engine can't frame; see req->content_length below)
 *           -2  request-target path >= sizeof(req->path) (256)  -> caller sends 414
 *           -3  retired: used to mean "a header name/value would not fit its fixed-size copy",
 *               impossible now that req->headers holds views into `raw` instead of copies - any header
 *               that made it into a complete request already fits inside the whole header block, which
 *               was always capped independently (431 if the block itself exceeds BUF_SIZE, checked by
 *               the caller before this is ever reached). Kept reserved, not reused for something else,
 *               so old caller code branching on it is merely dead, never wrong. (This is this function's
 *               own top-level return code; req->content_length has an unrelated -3 of its own, below.)
 *           -4  a percent-decoded path, query name or query value contains an embedded NUL -> caller
 *               sends 400 ("%00" - or a raw NUL byte - used to silently truncate everything read
 *               through it as a C string, e.g. "/style.css%00.png" looking like "/style.css" to a
 *               suffix check; never silently truncated now)
 *   On -1, req->content_length == -2 means "body too large"      -> caller sends 413.
 *   On -1, req->content_length == -3 means a Transfer-Encoding this engine can't frame (anything
 *   other than exactly the single token "chunked" - a substring lookalike like "xchunked", "chunked"
 *   alongside any other coding regardless of order, or an unsupported coding alone, e.g. "gzip") ->
 *   caller sends 501 (the server understood the request but can't process that transfer-coding).
 *   Ownership: on success req->body is allocated from `arena` (content_length + 1 bytes, NUL-terminated,
 *   never NULL, may hold NUL bytes: use content_length, not strlen). Nobody free()s it: it is reclaimed
 *   when the arena is reset (the engine passes Connection.arena; tests pass their own Arena). It is NULL
 *   after any failure. `req->arena` is also set to this same arena: req_get_header/req_get_cookie
 *   use it to materialize NUL-terminated strings out of the views/lazy split below. Only the fields
 *   behind *_count (and the scalars) are initialized; read arrays through the accessors.
 *   Decoding: req->path is percent-decoded; req->query stays raw; query names/values are
 *   percent- and '+'-decoded; header and cookie values are not decoded.
 *   Storage: req->headers holds VIEWS into `raw` (name/value point into it, not NUL-terminated,
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
 *   (closes a proxy-desync ambiguity a strict CRLF-only reader would not share).
 */
int parse_http_request(const char *raw, size_t raw_len, Request *req, Arena *arena);

/*
 * parse_request_head: runs phr_parse_request once over buf[0..len) and fills head with the raw
 * tokenization (method/path/headers) and the framing verdict (content-length/chunked, same codes
 * request_framing returns) so a caller that needs more than one of those - the body-limit check, the
 * completeness check, the full parse - doesn't pay for its own pass over the same bytes.
 * head->header_len == 0 means incomplete or malformed (distinguished only by this function's return
 * value, exactly as request_framing already worked): every other field is then not meaningful.
 * a head picohttpparser still calls incomplete although buf already holds a blank line (e.g. a
 * version token under 9 bytes, "GET / X\r\n\r\n") is malformed (-1), never incomplete.
 * picohttpparser runs only once buf holds a blank line: before that the head is incomplete (0), even
 * when its request line or a header is already malformed (answered 400 once the blank line arrives).
 * request_framing and request_is_complete are now thin wrappers over this plus request_head_is_complete;
 * they keep their own signatures for existing callers and are unaffected in behavior.
 */
int parse_request_head(const char *buf, size_t len, ParsedHead *head);

/*
 * parse_request_head_resume: parse_request_head, but the search for the head's terminating blank line
 * resumes from *head_scan (an offset into buf, 0 = from the start) and advances it while none is found,
 * so calling it once per recv as the SAME request's buf grows costs linear, not quadratic, time in the
 * head size. Same result as parse_request_head for every buf. *head_scan is per request (reset to 0 when
 * the request start moves, e.g. Connection.head_scan) and must never exceed a len it was used with.
 */
int parse_request_head_resume(const char *buf, size_t len, ParsedHead *head, size_t *head_scan);

/*
 * request_head_is_complete: the request_is_complete logic against an already-parsed head instead of
 * re-parsing. head must come from parse_request_head over buf[0..len). Same return convention as
 * request_is_complete.
 * chunk_scan (may be NULL): resume state for a chunked body, carried across calls for the SAME
 * request as buf grows (zeroed before its first call, e.g. Connection.chunk_scan) - each body byte is
 * then scanned once in total instead of once per call. NULL scans from the body start every time.
 */
int request_head_is_complete(const ParsedHead *head, const char *buf, size_t len, ChunkScanState *chunk_scan);

/*
 * request_head_expects_continue: 1 when a complete head asks for "100 Continue" before its body -
 * an "Expect" header whose value is "100-continue" (case-insensitive, OWS-trimmed), on an HTTP/1.1+
 * request with a valid framing that has a body to wait for (Content-Length > 0, or chunked). 0 for
 * anything else, including HTTP/1.0 (a 1xx must not be sent to it) and an incomplete or malformed head.
 * Other expectations are ignored (no 417). Pure: reads head only.
 */
int request_head_expects_continue(const ParsedHead *head);

/*
 * request_wire_len: bytes the request occupies on the wire - headers plus its framed body, so
 * buf + request_wire_len is where a pipelined next request starts. Valid only after
 * request_head_is_complete returned 1 for a head with header_len > 0 and content_length >= 0 (a
 * well-framed request); for a chunked body chunk_scan must be the state that call used (it reads
 * chunk_scan->body_end). Anything after a Content-Length body is never part of it.
 */
size_t request_wire_len(const ParsedHead *head, const ChunkScanState *chunk_scan);

/*
 * parse_http_request_from_head: the population half of parse_http_request, given an already-parsed
 * head (parse_request_head) instead of re-running phr_parse_request/request_framing on the same bytes
 *. The caller must already have confirmed the request is ready to parse via request_head_is_complete
 * (true for both a genuinely complete head and a malformed request line - never call this while it
 * still returns false). head->header_len == 0 then means the request line itself was malformed (not
 * "incomplete" - that case never reaches here): returns -1 immediately, before touching head->method/path,
 * which are NULL in that case. Same return codes, ownership and limits as parse_http_request otherwise,
 * which is now a thin wrapper: parse_request_head + this.
 */
int parse_http_request_from_head(const char *raw, size_t raw_len, const ParsedHead *head, Request *req,
                                 Arena *arena);

/*
 * parse_http_request_in_place: parse_http_request_from_head without the body copy - the engine's
 * path (connection.c). req->body points INTO raw at raw + head->header_len; a chunked body is decoded
 * in place over its own framing (raw is modified). One '\0' is written at req->body[content_length]:
 * for a Content-Length body that is the byte just past the request (raw[raw_len] when nothing follows,
 * so raw[raw_len] must be writable), and *saved_byte_out receives what was there - the caller restores
 * it once the body is no longer needed (before anything else parses raw again, e.g. a pipelined next
 * request). Written only on success (0); same return codes and limits as parse_http_request otherwise.
 * The body is valid only while raw is: never past the handler's return.
 */
int parse_http_request_in_place(char *raw, size_t raw_len, const ParsedHead *head, Request *req, Arena *arena,
                               char *saved_byte_out);

/*
 * request_framing: locate the header block and decide how the body is framed.
 *   *header_len_out = bytes up to and including "\r\n\r\n", or 0 if headers are incomplete or malformed
 *     (a bare '\n' line ending anywhere, or a malformed request line, leave this at 0 too;
 *     told apart from "incomplete" only by the return value, see request_head_is_complete).
 *   *chunked_out    = 1 only when the Transfer-Encoding value's sole comma-separated token (OWS trimmed)
 *     is "chunked", case-insensitive (not a substring search - "xchunked" or "chunked, gzip" don't
 *     count).
 *   *path_out / *path_len_out (both optional, pass NULL to skip) = the raw (not percent-decoded)
 *     request-target and its length, pointing into `buf`; valid only when the return value's
 *     caller can confirm *header_len_out > 0 (undefined content otherwise). Raw: pass it through
 *     request_target_path before matching it against anything.
 *   Returns the Content-Length code: 0 absent/zero, >0 value, -1 malformed / conflicting
 *   duplicates / chunked+Content-Length, -2 above MAX_BODY_SIZE, -3 Transfer-Encoding names
 *   anything other than exactly the single token "chunked" -> caller sends 501.
 * Header names match exactly (case-insensitive) at line start: "X-Content-Length" never matches.
 */
int request_framing(const char *buf, size_t len, size_t *header_len_out, int *chunked_out,
                    const char **path_out, size_t *path_len_out);

/*
 * request_is_complete: 1 when buf[0..len) holds a full request (headers + framed body), when framing is
 * invalid/oversized, or when the request line/header block itself is malformed - "HTTP/2.0", garbage, no
 * HTTP version - in every one of those cases parse_http_request reports the specific error.
 * 0 means "read more bytes", and only ever for one of two cases (asserted by fuzz_parser.c and
 * test_answered.c): no blank line has arrived yet, or a well-framed head waits on a body that is not all
 * there. Once 1 for a prefix, 1 for every longer buffer. Called after every recv().
 */
int request_is_complete(const char *buf, size_t len);

/* Content-Length code (see request_framing) of a NUL-terminated header block or raw request. */
int extract_content_length(const char *header_block);

/* 1 when a Transfer-Encoding header of a NUL-terminated header block has "chunked" as its sole
 * comma-separated token (token-exact, not a substring search - "xchunked" and "gzip, chunked" are 0). */
int request_has_chunked_encoding(const char *header_block);

/* 1 if the connection must close after this response: "Connection: close" wins, then
 * "Connection: keep-alive", else HTTP/1.1 keeps alive and everything else closes. */
int request_wants_close(const Request *req);

/*
 * Copies what a handler can read from `src` into `dst` without touching unused slots (Request is ~10 KB,
 * mostly fixed arrays read only up to their *_count): the strings, params, query and cookie entries in
 * use, the header views, body and content_length. Views (header name/value, body) that point into
 * [old_base, old_base + old_len) are moved to the same offset from new_base; anything else is kept as is.
 * dst->arena = arena (where req_get_header materializes from now on). Pure: nothing else is written.
 */
void request_clone_used(Request *dst, const Request *src, const char *old_base, size_t old_len, char *new_base,
                        Arena *arena);

/*
 * Chunked request bodies (RFC 7230 4.1). chunked_body_scan validates framing without copying:
 *   1 complete (*decoded_len_out = decoded size), 0 need more bytes,
 *  -1 malformed framing, -2 decoded size would exceed max_decoded_len.
 * A size line is 1*HEXDIG (at most 16 digits) then nothing or BWS ";" chunk-ext - never "0x5", "+5" or
 * " 5". Trailer lines may hold no bare CR/LF and no control character but HTAB.
 * chunked_body_decode requires a prior scan result of 1 on the same input; `out` needs
 * decoded_len bytes and may be body_start itself (in-place decode); returns bytes written
 * (binary-safe: never strlen the result).
 * chunked_body_scan_resume: same result codes, but continues from *state (zeroed = body start)
 * and advances it past every fully validated chunk, so calling it again after more bytes are
 * appended to the same body costs only the new bytes (plus one re-read size line). body_start and
 * the bytes already scanned must be unchanged between calls (offsets, so a realloc'd copy is fine).
 * On 1 it also sets state->body_end: the body's full wire length including trailers.
 * chunked_body_scan is the from-scratch wrapper.
 */
int chunked_body_scan(const char *body_start, size_t available, size_t max_decoded_len, size_t *decoded_len_out);
int chunked_body_scan_resume(const char *body_start, size_t available, size_t max_decoded_len,
                             ChunkScanState *state, size_t *decoded_len_out);
size_t chunked_body_decode(const char *body_start, size_t available, char *out);

/* Component parsers (called by parse_http_request; exposed for tests). Each resets its own *_count.
 * parse_query_string returns 0 ok, -1 if a decoded name/value contains an embedded NUL - req is
 * still fully populated up to and including the offending pair, same "don't bother finishing what the
 * caller will reject anyway" convention as parse_http_request's own -4.
 * parse_headers fills req->headers with VIEWS into header_block, not copies - same storage
 * req_get_header expects, so it needs `arena` for the same reason parse_http_request does: materializing
 * a NUL-terminated value on a later req_get_header call. header_block must outlive any such call. */
int parse_query_string(const char *query, Request *req);    /* "a=1&b=2"; bare key -> "" */
void parse_headers(const char *header_block, Request *req, Arena *arena);  /* "Name: value\r\n..." */
void parse_cookies(const char *cookie_header, Request *req); /* NULL ok; "a=1; b=2" */

/* Accessors: first match, NULL when absent. Returned pointers live as long as `req`.
 * req_get_header materializes a NUL-terminated copy of the matching view into req->arena on every
 * call (not cached); returns NULL without allocating if req->arena is NULL (a Request no parse function
 * ever populated). req_get_cookie runs parse_cookies itself, once, the first time it's called for
 * a given req (req->cookies_parsed) - a request whose Cookie header nobody reads never pays for the
 * split at all. */
const char *req_get_query(const Request *req, const char *name);  /* case-sensitive, decoded */
const char *req_get_header(const Request *req, const char *name); /* case-INsensitive name */
const char *req_get_cookie(const Request *req, const char *name); /* case-sensitive, not decoded */

/* Percent-decode src into dst (dst_size incl. NUL, truncates). dst may equal src. '+' -> ' ' only if decode_plus.
 * A '%' without two hex digits is copied literally. Returns 0 ok, -1 if a decoded byte is NUL -
 * dst is still fully written and NUL-terminated, but the caller should treat it as invalid input. */
int url_decode(const char *src, char *dst, size_t dst_size, int decode_plus);
/* url_decode over src[0..src_len) (not NUL-terminated). Returns 0 ok, -1 a decoded byte is NUL,
 * -2 the decoded bytes did not fit dst (dst holds the truncated prefix). Either error: invalid input. */
int url_decode_span(const char *src, size_t src_len, char *dst, size_t dst_size, int decode_plus);

/* one path shape for routing, middleware and body limits. In place, on an already-decoded path:
 * collapses repeated '/', keeps at most one trailing '/'. Returns -1 (caller answers 400) for a path
 * not starting with '/' (other than exactly "*"), or containing a "." or ".." segment. Never grows. */
int path_canonicalize(char *path);

/* request-target -> the canonical path every matcher sees (routing, middleware prefixes, body
 * limits): query string dropped, percent-decoded, then path_canonicalize. Pure; writes out (NUL-
 * terminated) even on failure. Returns 0 ok, -2 path part (before '?') does not fit out_size (414),
 * -4 an encoded '/' ("%2F"), a decoded NUL, a "."/".." segment or no leading '/' (400). */
int request_target_path(const char *target, size_t target_len, char *out, size_t out_size);

/* canonical registration form of a middleware/body-limit/mount prefix: "" (matches everything) or
 * "/seg[/seg...]" - leading '/' added, repeated and trailing '/' removed, so "admin/", "/admin/" and
 * "//admin" all become "/admin". Truncates at a segment boundary to fit out_size. */
void path_normalize_prefix(const char *prefix, char *out, size_t out_size);

/* 1 when a normalized prefix (path_normalize_prefix) covers a canonical path at a segment
 * boundary: "" matches everything; "/api" matches "/api" and "/api/x", not "/apiary". */
int path_prefix_matches(const char *prefix, const char *path);

/* Reason phrase for the 28 statuses the engine knows; "Unknown" otherwise. */
const char *status_text(int status);

/* an IMF-fixdate ("Sun, 06 Nov 1994 08:49:37 GMT", RFC 9110 5.6.7) is exactly HTTP_DATE_LEN chars. */
#define HTTP_DATE_LEN 29

/* Writes `t` (seconds since the epoch, UTC) as an IMF-fixdate into out (HTTP_DATE_LEN + 1 bytes, NUL-terminated).
 * Pure, locale-independent (no strftime / gmtime). Times before 1970 are clamped to the epoch. */
void format_http_date(time_t t, char out[HTTP_DATE_LEN + 1]);

/* the Date header value for second `now`, from a per-process one-entry cache refreshed only when the second
 * changes (so formatting costs once a second, not per response). The pointer stays valid; its text changes on the
 * next call with a different second. Not thread-safe - the engine is single-threaded per worker process. */
const char *http_date_for(time_t now);

#endif /* HTTP_PARSER_H */
