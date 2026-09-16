#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include <stddef.h>
#include "app_types.h"

/*
 * Parses the "Content-Length:" value out of a header block: 0 if absent,
 * -1 if present but negative or larger than the server will ever buffer
 * (BUF_SIZE - 1), otherwise the value.
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
 */
int parse_http_request(const char *raw, Request *req);

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

const char *status_text(int status);

#endif /* HTTP_PARSER_H */
