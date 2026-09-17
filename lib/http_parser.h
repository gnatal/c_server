#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include <stddef.h>
#include "app_types.h"

/*
 * Parses the "Content-Length:" value out of a header block: 0 if absent,
 * the value otherwise - except two distinct rejection sentinels callers use
 * to pick an HTTP status: -1 for a malformed (negative) value (-> 400 Bad
 * Request), -2 for a well-formed value that exceeds MAX_BODY_SIZE (-> 413
 * Payload Too Large). Both are negative, so request_is_complete/
 * parse_http_request's generic "content_length < 0" checks still treat
 * either the same way (stop buffering / reject); connection.c is what
 * distinguishes -1 from -2 to choose the response status.
 */
int extract_content_length(const char *header_block);

/*
 * Pure check (no I/O) for whether a full HTTP request is already sitting in
 * buf: the "\r\n\r\n" header terminator must be present, and if a
 * Content-Length was given, that many body bytes must have arrived too.
 * Called after every non-blocking recv() as the buffer grows.
 */
int request_is_complete(const char *buf, size_t len);

/*
 * Decides whether the connection should close after this response: an
 * explicit "Connection: close" always wins, an explicit "Connection:
 * keep-alive" always keeps it open, and otherwise it falls back to the
 * HTTP version default (HTTP/1.1 keeps alive, anything else - HTTP/1.0 or
 * an unparsed request line - closes).
 */
int request_wants_close(const Request *req);

/*
 * Parses a raw HTTP request buffer into method, path, query, headers and
 * body. req->path is URL-decoded in place (decode_plus = 0) after the query
 * string is split off, so route matching/req_get_param and req->path itself
 * all see decoded bytes; req->query stays the raw, unparsed string, but the
 * req->query_names/query_values arrays parse_query_string fills from it are
 * decoded (see parse_query_string).
 *
 * raw_len is the number of real bytes sitting in raw (e.g. conn->in_len),
 * not derived from strlen(raw) internally - the request line and headers are
 * still located via the NUL-terminated string functions (strstr/sscanf,
 * legitimate since a request line/header block can't contain embedded NULs),
 * but the body is sized off raw_len so a body containing arbitrary bytes -
 * including embedded NULs, e.g. a binary file in a multipart/form-data part,
 * lib/multipart.h - is copied in full rather than being truncated at the
 * first NUL byte the way strlen(body_start) would.
 *
 * Returns 0 on success, -1 for most parse failures (malformed request line,
 * missing headers, invalid Content-Length -> connection.c sends 400, except
 * where it re-checks extract_content_length itself for -> 413), and -2
 * specifically for a request-line path longer than req->path (256 bytes)
 * can hold -> connection.c sends 414 URI Too Long rather than silently
 * truncating the path into a shorter one the client never asked for.
 *
 * req->body is malloc'd here, sized to exactly content_length + 1 bytes
 * (never more, even if raw holds trailing bytes past the declared body -
 * e.g. a pipelined next request) - the caller owns it and must free() it
 * once done with the Request, on every return path (a failed parse still
 * guarantees req->body is NULL - safe to free unconditionally). raw itself
 * is never modified (this stays a pure function only reading raw bytes and
 * writing out fields on req).
 */
int parse_http_request(const char *raw, size_t raw_len, Request *req);

/*
 * Pure: splits a raw query string ("a=1&b=2") on '&' then '=' into
 * req->query_names/query_values, bounded by MAX_QUERY_PARAMS (extra pairs
 * are dropped rather than overflowing the fixed arrays, same as
 * MAX_ROUTES/MAX_PARAMS elsewhere). A pair with no '=' (e.g. "flag") gets an
 * empty-string value. Each name and value is then run through url_decode
 * (decode_plus = 1, the application/x-www-form-urlencoded convention), so
 * "a%20b=c%2Bd" comes back as name "a b", value "c+d". Called by
 * parse_http_request; exposed separately so it can be unit-tested against a
 * query string directly.
 */
void parse_query_string(const char *query, Request *req);

/*
 * Pure percent-decoding of one URL component: "%XX" hex escapes become the
 * literal byte, and - when decode_plus is set - '+' becomes a literal space
 * (the application/x-www-form-urlencoded convention used for query-string
 * keys/values, not path segments, which is why parse_http_request decodes
 * req->path with decode_plus = 0). A '%' not followed by two hex digits is
 * copied through as-is rather than decoded. Decoded output is never longer
 * than src, so it's always safe to call with dst == src (in-place decode);
 * dst_size still bounds the write and truncates on overflow, same as
 * strncpy elsewhere.
 */
void url_decode(const char *src, char *dst, size_t dst_size, int decode_plus);

/* Looks up a parsed query-string value by key (e.g. req_get_query(req, "q")
 * for "?q=cats"), or NULL if that key wasn't present. */
const char *req_get_query(const Request *req, const char *name);

/*
 * Pure: splits a raw header block ("Name: value\r\nName2: value2") on
 * "\r\n" then the first ':' into req->header_names/header_values, bounded
 * by MAX_HEADERS (extra headers are dropped rather than overflowing the
 * fixed arrays, same as MAX_QUERY_PARAMS/MAX_PARAMS elsewhere). Leading
 * spaces after the ':' are trimmed; a line with no ':' is skipped rather
 * than stored. Not URL-decoded - header values pass through as-is. Called
 * by parse_http_request; exposed separately so it can be unit-tested
 * against a header block directly.
 */
void parse_headers(const char *header_block, Request *req);

/*
 * Looks up a parsed header value by name (e.g. req_get_header(req,
 * "Content-Type")), case-insensitively per RFC 7230 (header field names are
 * case-insensitive), or NULL if that header wasn't present. Returns the
 * first matching value if a header appears more than once.
 */
const char *req_get_header(const Request *req, const char *name);

/*
 * Pure: splits a raw "Cookie" header value ("a=1; b=2") on ';' then the
 * first '=' into req->cookie_names/cookie_values, bounded by MAX_COOKIES
 * (extra pairs are dropped rather than overflowing the fixed arrays, same
 * as MAX_QUERY_PARAMS/MAX_HEADERS elsewhere). Leading spaces after ';' are
 * trimmed; a pair with no '=' is skipped rather than stored. Not
 * URL-decoded, same as header values. cookie_header may be NULL (no
 * "Cookie" header present), treated the same as an empty string. Called by
 * parse_http_request (via req_get_header(req, "Cookie")); exposed
 * separately so it can be unit-tested against a header value directly.
 */
void parse_cookies(const char *cookie_header, Request *req);

/*
 * Looks up a parsed cookie value by name (e.g. req_get_cookie(req,
 * "session")), or NULL if that cookie wasn't sent. Case-sensitive per RFC
 * 6265 (strcmp), unlike req_get_header's case-insensitive header-name
 * lookup.
 */
const char *req_get_cookie(const Request *req, const char *name);

const char *status_text(int status);

/*
 * Pure: true when header_block's "Transfer-Encoding" header value contains
 * "chunked" (case-insensitive), scoped to just that header's own line (a
 * bounded stack copy, not scanned past it) so a "chunked" appearing
 * elsewhere - another header's value, or body bytes, when header_block is
 * actually the whole raw connection buffer - can't cause a false match.
 * Called both on the raw connection buffer, before headers are even sliced
 * out of it (request_is_complete), and on the already-isolated req->headers
 * block (parse_http_request) - the same dual-use pattern
 * extract_content_length already follows.
 */
int request_has_chunked_encoding(const char *header_block);

/*
 * Pure scan of a Transfer-Encoding: chunked request body (RFC 7230 4.1)
 * starting at body_start: walks "<hex-size>[;ext]\r\n<data>\r\n" chunks, up
 * to available bytes, until a 0-size last-chunk plus its trailer-part and
 * terminating CRLF is found. Never allocates or copies chunk data itself -
 * chunked_body_decode does that once this reports the body complete - it
 * only verifies chunk framing and tracks the running decoded size in
 * *decoded_len_out, so a caller can reject an oversized body
 * (max_decoded_len, e.g. MAX_BODY_SIZE) as soon as the chunk-size lines
 * declare more than that, without needing that much actual data to have
 * arrived first (mirrors extract_content_length's early rejection of a
 * too-large declared Content-Length).
 *
 * Returns:
 *   1  - fully received; *decoded_len_out is the total decoded body size.
 *   0  - not yet complete (need more bytes); *decoded_len_out is the
 *        decoded size of whole chunks confirmed so far.
 *  -1  - malformed chunk framing (non-hex chunk-size, a chunk's data not
 *        followed by its own CRLF, or a chunk-size line longer than
 *        MAX_CHUNK_SIZE_LINE_LEN can legitimately be).
 *  -2  - the running decoded size already exceeds max_decoded_len.
 */
int chunked_body_scan(const char *body_start, size_t available, size_t max_decoded_len, size_t *decoded_len_out);

/*
 * Pure: decodes a complete chunked body into out - a caller-owned buffer of
 * at least the decoded_len_out chunked_body_scan reported for this exact
 * body_start/available. The caller must have already gotten a 1 back from
 * chunked_body_scan on this same input; this does not re-validate framing,
 * same contract as parse_multipart_body trusting a prior successful
 * multipart_parse_boundary call. Returns the number of bytes written -
 * decoded chunk data can contain arbitrary bytes, including embedded NULs,
 * so callers must use this return value, never strlen(out), the same
 * convention multipart/urlencoded body parsing already follow.
 */
size_t chunked_body_decode(const char *body_start, size_t available, char *out);

#endif /* HTTP_PARSER_H */
