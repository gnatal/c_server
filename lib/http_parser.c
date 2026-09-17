#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "http_parser.h"

int extract_content_length(const char *header_block) {
    const char *cl_header = strcasestr(header_block, "Content-Length:");
    if (cl_header == NULL) {
        return 0;
    }
    const long value = atol(cl_header + strlen("Content-Length:"));
    if (value < 0) {
        return -1;
    }
    if (value > MAX_BODY_SIZE) {
        return -2;
    }
    return (int)value;
}

void url_decode(const char *src, char *dst, size_t dst_size, int decode_plus) {
    if (dst_size == 0) {
        return;
    }

    size_t out = 0;
    for (size_t i = 0; src[i] != '\0' && out + 1 < dst_size; i++) {
        if (src[i] == '%' && isxdigit((unsigned char)src[i + 1]) && isxdigit((unsigned char)src[i + 2])) {
            const char hex[3] = { src[i + 1], src[i + 2], '\0' };
            dst[out++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else if (src[i] == '+' && decode_plus) {
            dst[out++] = ' ';
        } else {
            dst[out++] = src[i];
        }
    }
    dst[out] = '\0';
}

int request_is_complete(const char *buf, size_t len) {
    const char *header_end = strstr(buf, "\r\n\r\n");
    if (header_end == NULL) {
        return 0;
    }

    size_t header_len = (size_t)((header_end + 4) - buf);
    size_t body_have = len - header_len;

    if (request_has_chunked_encoding(buf)) {
        size_t decoded_len;
        const int scan = chunked_body_scan(header_end + 4, body_have, MAX_BODY_SIZE, &decoded_len);
        /* Fully received (1) and any rejection (-1 malformed, -2 too large)
         * both stop buffering here - parse_http_request re-derives which
         * one happened and picks a response status, same pattern the
         * Content-Length branch below uses its own negative sentinels for. */
        return scan != 0;
    }

    const int content_length = extract_content_length(buf);
    if (content_length < 0) {
        /* Invalid Content-Length: stop buffering and let parse_http_request
         * reject it as a 400 rather than waiting for bytes that were never
         * validated to arrive. */
        return 1;
    }

    return body_have >= (size_t)content_length;
}

int request_wants_close(const Request *req) {
    if (strcasestr(req->headers, "Connection: close") != NULL) {
        return 1;
    }
    if (strcasestr(req->headers, "Connection: keep-alive") != NULL) {
        return 0;
    }
    return strcmp(req->version, "HTTP/1.1") != 0;
}

int parse_http_request(const char *raw, size_t raw_len, Request *req) {
    memset(req, 0, sizeof(*req));

    char full_path[512];
    if (sscanf(raw, "%7s %511s %15s", req->method, full_path, req->version) < 2) {
        return -1;
    }

    char *qmark = strchr(full_path, '?');
    if (qmark != NULL) {
        *qmark = '\0';
        strncpy(req->query, qmark + 1, sizeof(req->query) - 1);
    }

    /* full_path (sized 512 above, to comfortably hold a request-line token
     * before this check) can be longer than req->path (256) can hold -
     * reject outright (-> 414 URI Too Long, connection.c) rather than
     * silently truncating into a shorter path that isn't the one the client
     * asked for. */
    if (strlen(full_path) >= sizeof(req->path)) {
        return -2;
    }
    strncpy(req->path, full_path, sizeof(req->path) - 1);
    url_decode(req->path, req->path, sizeof(req->path), 0);
    parse_query_string(req->query, req);

    char *header_start = strstr(raw, "\r\n");
    char *header_end = strstr(raw, "\r\n\r\n");
    if (header_start == NULL || header_end == NULL) {
        return -1;
    }
    header_start += 2;

    /* A request line with zero header lines after it (request-line CRLF
     * immediately followed by the blank-line CRLF, e.g. "GET / HTTP/1.1\r\n\r\n")
     * makes the *first* "\r\n" in raw coincide with the start of that
     * "\r\n\r\n" terminator itself - header_start (+2) then lands 2 bytes
     * past header_end instead of at it. Without this clamp, the pointer
     * subtraction below wraps to a huge size_t (clamped to sizeof(headers)-1
     * next), turning an ordinary header-less request into an out-of-bounds
     * heap read via memcpy. Every request with at least one real header line
     * already has header_start <= header_end naturally, so this is a no-op
     * there. */
    if (header_start > header_end) {
        header_start = header_end;
    }

    size_t header_len = (size_t)(header_end - header_start);
    if (header_len >= sizeof(req->headers)) {
        header_len = sizeof(req->headers) - 1;
    }
    memcpy(req->headers, header_start, header_len);
    req->headers[header_len] = '\0';
    parse_headers(req->headers, req);
    parse_cookies(req_get_header(req, "Cookie"), req);

    char *body_start = header_end + 4;
    size_t consumed = (size_t)(body_start - raw);
    size_t available = raw_len > consumed ? raw_len - consumed : 0;

    if (request_has_chunked_encoding(req->headers)) {
        /* RFC 7230 3.3.3: a message with both Transfer-Encoding and
         * Content-Length is ambiguous about where the body actually ends -
         * a request-smuggling shape, not just a client mistake - so it's
         * rejected outright (400) rather than guessing which header to
         * trust. */
        if (req_get_header(req, "Content-Length") != NULL) {
            return -1;
        }

        size_t decoded_len = 0;
        const int scan = chunked_body_scan(body_start, available, MAX_BODY_SIZE, &decoded_len);
        if (scan == -2) {
            /* Reuse the same sentinel extract_content_length uses for a
             * too-large declared Content-Length, so connection.c's existing
             * "req.content_length == -2 -> 413" check covers this case too
             * without needing to know anything about chunked encoding. */
            req->content_length = -2;
            return -1;
        }
        if (scan != 1) {
            /* Malformed framing, or (shouldn't happen - request_is_complete
             * already required a full scan before parse_http_request is
             * ever called) still incomplete. */
            return -1;
        }

        req->body = malloc(decoded_len + 1);
        if (req->body == NULL) {
            return -1;
        }
        const size_t written = chunked_body_decode(body_start, available, req->body);
        req->body[written] = '\0';
        req->content_length = (int)written;

        return 0;
    }

    req->content_length = extract_content_length(req->headers);
    if (req->content_length < 0) {
        return -1;
    }

    /* body_start may hold more than content_length bytes (e.g. a pipelined
     * next request already sitting in the same buffer) - take exactly
     * content_length of it, never more, same as before this was heap-backed.
     * available comes from raw_len, not strlen(body_start): a body can
     * contain embedded NUL bytes (a binary file part in a multipart/
     * form-data body, lib/multipart.h) that strlen would stop at early,
     * silently truncating a request that was received in full. */
    size_t body_len = (size_t)req->content_length;
    if (body_len > available) {
        body_len = available;
    }

    req->body = malloc(body_len + 1);
    if (req->body == NULL) {
        return -1;
    }
    memcpy(req->body, body_start, body_len);
    req->body[body_len] = '\0';

    return 0;
}

void parse_query_string(const char *query, Request *req) {
    req->query_count = 0;
    if (query[0] == '\0') {
        return;
    }

    char buf[sizeof(req->query)];
    strncpy(buf, query, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *saveptr;
    char *pair = strtok_r(buf, "&", &saveptr);
    while (pair != NULL && req->query_count < MAX_QUERY_PARAMS) {
        char *eq = strchr(pair, '=');
        const char *value = "";
        if (eq != NULL) {
            *eq = '\0';
            value = eq + 1;
        }

        char *name_slot = req->query_names[req->query_count];
        char *value_slot = req->query_values[req->query_count];
        strncpy(name_slot, pair, sizeof(req->query_names[0]) - 1);
        name_slot[sizeof(req->query_names[0]) - 1] = '\0';
        strncpy(value_slot, value, sizeof(req->query_values[0]) - 1);
        value_slot[sizeof(req->query_values[0]) - 1] = '\0';
        url_decode(name_slot, name_slot, sizeof(req->query_names[0]), 1);
        url_decode(value_slot, value_slot, sizeof(req->query_values[0]), 1);
        req->query_count++;

        pair = strtok_r(NULL, "&", &saveptr);
    }
}

const char *req_get_query(const Request *req, const char *name) {
    for (int i = 0; i < req->query_count; i++) {
        if (strcmp(req->query_names[i], name) == 0) {
            return req->query_values[i];
        }
    }
    return NULL;
}

void parse_headers(const char *header_block, Request *req) {
    req->header_count = 0;
    if (header_block[0] == '\0') {
        return;
    }

    char buf[sizeof(req->headers)];
    strncpy(buf, header_block, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *saveptr;
    char *line = strtok_r(buf, "\r\n", &saveptr);
    while (line != NULL && req->header_count < MAX_HEADERS) {
        char *colon = strchr(line, ':');
        if (colon != NULL) {
            *colon = '\0';
            char *value = colon + 1;
            while (*value == ' ') {
                value++;
            }

            char *name_slot = req->header_names[req->header_count];
            char *value_slot = req->header_values[req->header_count];
            strncpy(name_slot, line, sizeof(req->header_names[0]) - 1);
            name_slot[sizeof(req->header_names[0]) - 1] = '\0';
            strncpy(value_slot, value, sizeof(req->header_values[0]) - 1);
            value_slot[sizeof(req->header_values[0]) - 1] = '\0';
            req->header_count++;
        }
        line = strtok_r(NULL, "\r\n", &saveptr);
    }
}

const char *req_get_header(const Request *req, const char *name) {
    for (int i = 0; i < req->header_count; i++) {
        if (strcasecmp(req->header_names[i], name) == 0) {
            return req->header_values[i];
        }
    }
    return NULL;
}

void parse_cookies(const char *cookie_header, Request *req) {
    req->cookie_count = 0;
    if (cookie_header == NULL || cookie_header[0] == '\0') {
        return;
    }

    char buf[sizeof(req->header_values[0])];
    strncpy(buf, cookie_header, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *saveptr;
    char *pair = strtok_r(buf, ";", &saveptr);
    while (pair != NULL && req->cookie_count < MAX_COOKIES) {
        while (*pair == ' ') {
            pair++;
        }

        char *eq = strchr(pair, '=');
        if (eq != NULL) {
            *eq = '\0';
            const char *value = eq + 1;

            char *name_slot = req->cookie_names[req->cookie_count];
            char *value_slot = req->cookie_values[req->cookie_count];
            strncpy(name_slot, pair, sizeof(req->cookie_names[0]) - 1);
            name_slot[sizeof(req->cookie_names[0]) - 1] = '\0';
            strncpy(value_slot, value, sizeof(req->cookie_values[0]) - 1);
            value_slot[sizeof(req->cookie_values[0]) - 1] = '\0';
            req->cookie_count++;
        }
        /* A pair with no '=' is malformed - skipped, same as parse_headers
         * skipping a header line with no ':'. */

        pair = strtok_r(NULL, ";", &saveptr);
    }
}

const char *req_get_cookie(const Request *req, const char *name) {
    for (int i = 0; i < req->cookie_count; i++) {
        if (strcmp(req->cookie_names[i], name) == 0) {
            return req->cookie_values[i];
        }
    }
    return NULL;
}

const char *status_text(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 410: return "Gone";
        case 413: return "Payload Too Large";
        case 414: return "URI Too Long";
        case 415: return "Unsupported Media Type";
        case 422: return "Unprocessable Entity";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        default:  return "Unknown";
    }
}

int request_has_chunked_encoding(const char *header_block) {
    const char *te = strcasestr(header_block, "Transfer-Encoding:");
    if (te == NULL) {
        return 0;
    }
    te += strlen("Transfer-Encoding:");

    const char *line_end = strstr(te, "\r\n");
    size_t line_len = line_end != NULL ? (size_t)(line_end - te) : strlen(te);

    /* Bounded to just this header's own line - not scanned past it into
     * whatever follows (more headers, or body bytes, when header_block is
     * actually the whole raw connection buffer, see request_is_complete) -
     * via a fixed stack copy, same shape as parse_headers' per-line
     * handling. A value this long is already nonsensical for this header. */
    char line[128];
    if (line_len >= sizeof(line)) {
        line_len = sizeof(line) - 1;
    }
    memcpy(line, te, line_len);
    line[line_len] = '\0';

    return strcasestr(line, "chunked") != NULL;
}

/* Bare "\n" preceded by "\r", within the first len bytes of buf - the
 * chunk-framing line terminator (RFC 7230 4.1's chunk-size/trailer lines use
 * the same CRLF convention header lines do). Bounded by len rather than a
 * NUL terminator, since a chunk's own data bytes (opaque, possibly binary -
 * a chunked file upload) sit right next to this line in the same buffer and
 * must never be scanned into. Returns a pointer to the '\r', or NULL if no
 * such CRLF appears in the first len bytes. */
static const char *find_crlf(const char *buf, size_t len) {
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n') {
            return buf + i;
        }
    }
    return NULL;
}

/* The 4-byte "\r\n\r\n" run terminating a chunked body's trailer-part (RFC
 * 7230 4.1.2) - structurally identical to how the main header block ends
 * (see parse_http_request), so the same search finds it whether there are
 * zero trailer lines ("0\r\n\r\n", one contiguous run starting one byte into
 * the last-chunk line) or one or more ("0\r\nName: value\r\n\r\n", the run
 * right before the final blank line). No chunked trailer value in practice
 * should contain a literal "\r\n\r\n" - the same assumption request parsing
 * already makes when locating the end of the main header block. */
static const char *find_double_crlf(const char *buf, size_t len) {
    for (size_t i = 0; i + 4 <= len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return buf + i;
        }
    }
    return NULL;
}

int chunked_body_scan(const char *body_start, size_t available, size_t max_decoded_len, size_t *decoded_len_out) {
    size_t pos = 0;
    size_t decoded_len = 0;

    for (;;) {
        if (pos >= available) {
            *decoded_len_out = decoded_len;
            return 0;
        }

        const char *line_start = body_start + pos;
        const char *line_end = find_crlf(line_start, available - pos);
        if (line_end == NULL) {
            *decoded_len_out = decoded_len;
            /* A chunk-size line can't legitimately be this long with no
             * CRLF in sight yet - same reasoning as the 431 header-overflow
             * guard elsewhere, at a much smaller scale appropriate to one
             * framing line. */
            return (available - pos > MAX_CHUNK_SIZE_LINE_LEN) ? -1 : 0;
        }

        size_t line_len = (size_t)(line_end - line_start);
        if (line_len == 0 || line_len > MAX_CHUNK_SIZE_LINE_LEN) {
            *decoded_len_out = decoded_len;
            return -1;
        }

        /* Chunk-size is hex digits, optionally followed by ";ext" (chunk
         * extensions - accepted but ignored, same as most real servers). */
        char size_buf[MAX_CHUNK_SIZE_LINE_LEN + 1];
        memcpy(size_buf, line_start, line_len);
        size_buf[line_len] = '\0';
        char *ext = strchr(size_buf, ';');
        if (ext != NULL) {
            *ext = '\0';
        }
        if (size_buf[0] == '\0') {
            *decoded_len_out = decoded_len;
            return -1;
        }

        char *endptr;
        unsigned long chunk_size = strtoul(size_buf, &endptr, 16);
        if (*endptr != '\0') {
            *decoded_len_out = decoded_len;
            return -1; /* non-hex characters in the size */
        }

        if (chunk_size == 0) {
            /* Last-chunk: search for the trailer terminator starting at
             * this line's own start (line_start), not past it - see
             * find_double_crlf's comment for why that covers both the
             * zero-trailer and with-trailer shapes. */
            const char *terminator = find_double_crlf(line_start, available - pos);
            *decoded_len_out = decoded_len;
            return terminator != NULL ? 1 : 0;
        }

        /* Overflow-safe cap check: decoded_len never exceeds max_decoded_len
         * across iterations (this is the only place it grows), so
         * max_decoded_len - decoded_len can't underflow here. Checked
         * against the *declared* chunk_size, before its data has
         * necessarily all arrived - same early-rejection shape
         * extract_content_length gives a too-large declared Content-Length. */
        if (chunk_size > max_decoded_len - decoded_len) {
            *decoded_len_out = decoded_len;
            return -2;
        }

        pos += line_len + 2;
        if (pos + chunk_size + 2 > available) {
            *decoded_len_out = decoded_len;
            return 0; /* this chunk's data/trailing CRLF hasn't fully arrived yet */
        }

        /* Verify (not just assume) this chunk is properly CRLF-terminated -
         * a chunk-size that doesn't match the actual data length would
         * otherwise silently desync every subsequent chunk boundary. */
        if (body_start[pos + chunk_size] != '\r' || body_start[pos + chunk_size + 1] != '\n') {
            *decoded_len_out = decoded_len;
            return -1;
        }

        decoded_len += chunk_size;
        pos += chunk_size + 2;
    }
}

size_t chunked_body_decode(const char *body_start, size_t available, char *out) {
    size_t pos = 0;
    size_t out_len = 0;

    for (;;) {
        const char *line_start = body_start + pos;
        const char *line_end = find_crlf(line_start, available - pos);
        size_t line_len = (size_t)(line_end - line_start);

        char size_buf[MAX_CHUNK_SIZE_LINE_LEN + 1];
        memcpy(size_buf, line_start, line_len);
        size_buf[line_len] = '\0';
        char *ext = strchr(size_buf, ';');
        if (ext != NULL) {
            *ext = '\0';
        }
        unsigned long chunk_size = strtoul(size_buf, NULL, 16);

        pos += line_len + 2;
        if (chunk_size == 0) {
            /* Last-chunk reached - any trailer header lines are discarded,
             * same as multipart part headers beyond name/filename/
             * content-type aren't surfaced to callers either. */
            break;
        }

        memcpy(out + out_len, body_start + pos, chunk_size);
        out_len += chunk_size;
        pos += chunk_size + 2;
    }

    return out_len;
}
