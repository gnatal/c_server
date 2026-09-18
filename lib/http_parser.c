#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "http_parser.h"

static void parse_headers_n(const char *block, size_t len, Request *req);

/* ---- byte-level helpers (all bounded by an explicit length) ---- */

static int is_ows(const char c) {
    return c == ' ' || c == '\t';
}

static int hex_value(const char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Copies src[0..len) into dst, truncating to dst_size-1 bytes; always NUL-terminates. */
static void copy_bounded(char *dst, const size_t dst_size, const char *src, size_t len) {
    if (len >= dst_size) {
        len = dst_size - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* Percent-decodes src[0..src_len) into dst (dst_size bytes incl. NUL). dst may equal src:
 * output is never longer than input, so the write cursor never overtakes the read cursor. */
static void decode_bounded(char *dst, const size_t dst_size, const char *src, const size_t src_len,
                           const int decode_plus) {
    if (dst_size == 0) {
        return;
    }
    size_t out = 0;
    for (size_t i = 0; i < src_len && out + 1 < dst_size; i++) {
        if (src[i] == '%' && i + 2 < src_len && hex_value(src[i + 1]) >= 0 && hex_value(src[i + 2]) >= 0) {
            dst[out++] = (char)((hex_value(src[i + 1]) << 4) | hex_value(src[i + 2]));
            i += 2;
        } else if (src[i] == '+' && decode_plus) {
            dst[out++] = ' ';
        } else {
            dst[out++] = src[i];
        }
    }
    dst[out] = '\0';
}

void url_decode(const char *src, char *dst, const size_t dst_size, const int decode_plus) {
    decode_bounded(dst, dst_size, src, strlen(src), decode_plus);
}

/* Pointer to the "\r\n\r\n" ending the header block in buf[0..len), or NULL. Hops between '\n'
 * bytes with memchr (header lines are short) instead of a generic substring search. */
static const char *find_header_end(const char *buf, const size_t len) {
    const char *p = buf;
    const char *end = buf + len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        if (nl == NULL) {
            return NULL;
        }
        if (nl - buf >= 3 && nl[-1] == '\r' && nl[-2] == '\n' && nl[-3] == '\r') {
            return nl - 3;
        }
        p = nl + 1;
    }
    return NULL;
}

/* End of the header line starting at p: pointer to its "\r\n", or `end` if the
 * line is unterminated. Never reads outside [p, end). */
static const char *find_line_end(const char *p, const char *end) {
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        if (nl == NULL) {
            return end;
        }
        if (nl > p && nl[-1] == '\r') {
            return nl - 1;
        }
        p = nl + 1;
    }
    return end;
}

/* True when the comma-separated header value contains token (case-insensitive). */
static int list_has_token(const char *value, const char *token) {
    const size_t token_len = strlen(token);
    const char *p = value;
    while (*p != '\0') {
        while (*p == ',' || is_ows(*p)) {
            p++;
        }
        const char *start = p;
        while (*p != '\0' && *p != ',' && !is_ows(*p)) {
            p++;
        }
        if ((size_t)(p - start) == token_len && strncasecmp(start, token, token_len) == 0) {
            return 1;
        }
    }
    return 0;
}

/* ---- message framing (Content-Length / Transfer-Encoding) ---- */

/*
 * Line-anchored scan of a header block for the two framing headers. A header
 * counts only when its *name* (text before the first ':') equals the header
 * name exactly (case-insensitive), so "X-Content-Length: 9" or a request-line
 * target containing "Content-Length:" never match. Stops at the first blank
 * line, so a raw buffer that continues into a body is safe to pass.
 *
 * Returns the Content-Length code: 0 absent-or-zero, >0 value, -1 malformed or
 * conflicting duplicates, -2 above MAX_BODY_SIZE. *has_cl_out / *chunked_out
 * report header presence.
 */
static int scan_framing(const char *block, const size_t len, int *has_cl_out, int *chunked_out) {
    const char *p = block;
    const char *end = block + len;
    int result = 0;
    int has_cl = 0;
    int chunked = 0;

    while (p < end) {
        const char *le = find_line_end(p, end);
        if (le == p) {
            break; /* blank line: end of the header block */
        }
        const char *colon = memchr(p, ':', (size_t)(le - p));
        if (colon != NULL) {
            const size_t name_len = (size_t)(colon - p);
            const char *v = colon + 1;
            while (v < le && is_ows(*v)) {
                v++;
            }

            if (name_len == 14 && strncasecmp(p, "Content-Length", 14) == 0) {
                int value = 0;
                if (v < le && *v == '-') {
                    value = -1;
                } else if (v == le || *v < '0' || *v > '9') {
                    value = -1;
                } else {
                    long long acc = 0;
                    while (v < le && *v >= '0' && *v <= '9') {
                        if (acc <= MAX_BODY_SIZE) {
                            acc = acc * 10 + (*v - '0');
                        }
                        v++;
                    }
                    while (v < le && is_ows(*v)) {
                        v++;
                    }
                    if (v != le) {
                        value = -1; /* trailing garbage, e.g. "5abc" or "5, 5" */
                    } else if (acc > MAX_BODY_SIZE) {
                        value = -2;
                    } else {
                        value = (int)acc;
                    }
                }

                if (has_cl && value != result) {
                    result = -1; /* conflicting duplicate Content-Length headers */
                } else if (!has_cl) {
                    result = value;
                }
                has_cl = 1;
            } else if (name_len == 17 && strncasecmp(p, "Transfer-Encoding", 17) == 0) {
                for (const char *q = v; (size_t)(le - q) >= 7; q++) {
                    if (strncasecmp(q, "chunked", 7) == 0) {
                        chunked = 1;
                        break;
                    }
                }
            }
        }
        if (le >= end) {
            break;
        }
        p = le + 2;
    }

    if (has_cl_out != NULL) {
        *has_cl_out = has_cl;
    }
    if (chunked_out != NULL) {
        *chunked_out = chunked;
    }
    return result;
}

int extract_content_length(const char *header_block) {
    return scan_framing(header_block, strlen(header_block), NULL, NULL);
}

int request_has_chunked_encoding(const char *header_block) {
    int chunked = 0;
    scan_framing(header_block, strlen(header_block), NULL, &chunked);
    return chunked;
}

int request_framing(const char *buf, const size_t len, size_t *header_len_out, int *chunked_out) {
    *header_len_out = 0;
    *chunked_out = 0;

    const char *terminator = find_header_end(buf, len);
    if (terminator == NULL) {
        return 0;
    }
    *header_len_out = (size_t)(terminator + 4 - buf);

    const char *first_line_end = memchr(buf, '\r', (size_t)(terminator - buf));
    const char *block = first_line_end != NULL ? first_line_end + 2 : terminator;
    if (block > terminator) {
        block = terminator; /* zero header lines: the request line's CRLF starts the terminator */
    }

    int has_cl = 0;
    const int content_length = scan_framing(block, (size_t)(terminator - block), &has_cl, chunked_out);
    if (*chunked_out && has_cl) {
        return -1; /* RFC 7230 3.3.3: request-smuggling shape, rejected by parse_http_request */
    }
    return content_length;
}

int request_is_complete(const char *buf, const size_t len) {
    size_t header_len;
    int chunked;
    const int content_length = request_framing(buf, len, &header_len, &chunked);
    if (header_len == 0) {
        return 0;
    }
    if (content_length < 0) {
        return 1; /* stop buffering; parse_http_request turns this into 400/413 */
    }

    const size_t body_have = len - header_len;
    if (chunked) {
        size_t decoded_len;
        /* 1 = complete, -1/-2 = rejected: all stop buffering. Only 0 means "need more". */
        return chunked_body_scan(buf + header_len, body_have, MAX_BODY_SIZE, &decoded_len) != 0;
    }
    return body_have >= (size_t)content_length;
}

int request_wants_close(const Request *req) {
    const char *connection = req_get_header(req, "Connection");
    if (connection != NULL) {
        if (list_has_token(connection, "close")) {
            return 1;
        }
        if (list_has_token(connection, "keep-alive")) {
            return 0;
        }
    }
    return strcmp(req->version, "HTTP/1.1") != 0;
}

/* ---- request parsing ---- */

/* Skips spaces from p (bounded by end), returns the first non-space. */
static const char *skip_spaces(const char *p, const char *end) {
    while (p < end && *p == ' ') {
        p++;
    }
    return p;
}

int parse_http_request(const char *raw, const size_t raw_len, Request *req) {
    /* Initialize only what accessors read. The arrays behind the *_count fields
     * are never read past their count and every slot is NUL-terminated when
     * written, so zeroing the whole ~19 KB struct per request is unnecessary. */
    req->method[0] = '\0';
    req->path[0] = '\0';
    req->query[0] = '\0';
    req->version[0] = '\0';
    req->param_count = 0;
    req->query_count = 0;
    req->header_count = 0;
    req->cookie_count = 0;
    req->content_length = 0;
    req->body = NULL;

    const char *header_end = find_header_end(raw, raw_len);
    if (header_end == NULL) {
        return -1;
    }
    const char *line_end = find_line_end(raw, header_end + 2);
    if (line_end >= header_end + 2) {
        return -1; /* no CRLF-terminated request line */
    }

    /* Request line: METHOD SP target [SP version] */
    const char *p = raw;
    const char *sp = memchr(p, ' ', (size_t)(line_end - p));
    if (sp == NULL || sp == p || (size_t)(sp - p) >= sizeof(req->method)) {
        return -1; /* no method, or a method token that would not fit (was silently truncated) */
    }
    copy_bounded(req->method, sizeof(req->method), p, (size_t)(sp - p));

    const char *target = skip_spaces(sp, line_end);
    const char *target_end = memchr(target, ' ', (size_t)(line_end - target));
    if (target_end == NULL) {
        target_end = line_end;
    }
    if (target_end == target) {
        return -1;
    }

    const char *version = skip_spaces(target_end, line_end);
    const char *version_end = memchr(version, ' ', (size_t)(line_end - version));
    if (version_end == NULL) {
        version_end = line_end;
    }
    copy_bounded(req->version, sizeof(req->version), version, (size_t)(version_end - version));

    const char *qmark = memchr(target, '?', (size_t)(target_end - target));
    const char *path_end = qmark != NULL ? qmark : target_end;
    if ((size_t)(path_end - target) >= sizeof(req->path)) {
        return -2; /* -> 414 URI Too Long: never silently truncate into a different path */
    }
    copy_bounded(req->path, sizeof(req->path), target, (size_t)(path_end - target));
    url_decode(req->path, req->path, sizeof(req->path), 0);
    if (qmark != NULL) {
        copy_bounded(req->query, sizeof(req->query), qmark + 1, (size_t)(target_end - qmark - 1));
    }
    parse_query_string(req->query, req);

    /* Header block: after the request line's CRLF, up to the blank line. */
    const char *header_start = line_end + 2;
    if (header_start > header_end) {
        header_start = header_end; /* zero header lines: request-line CRLF is the start of "\r\n\r\n" */
    }
    const size_t header_block_len = (size_t)(header_end - header_start);
    parse_headers_n(header_start, header_block_len, req);
    parse_cookies(req_get_header(req, "Cookie"), req);

    const char *body_start = header_end + 4;
    const size_t available = raw_len - (size_t)(body_start - raw);

    int has_cl = 0;
    int chunked = 0;
    const int cl = scan_framing(header_start, header_block_len, &has_cl, &chunked);

    if (chunked) {
        if (has_cl) {
            return -1; /* RFC 7230 3.3.3: ambiguous framing, reject rather than guess */
        }
        size_t decoded_len = 0;
        const int scan = chunked_body_scan(body_start, available, MAX_BODY_SIZE, &decoded_len);
        if (scan == -2) {
            req->content_length = -2; /* connection.c maps -2 to 413 for both framings */
            return -1;
        }
        if (scan != 1) {
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

    req->content_length = cl;
    if (cl < 0) {
        return -1;
    }

    /* Copy exactly content_length bytes (a pipelined next request may follow in
     * `raw`). Sized off raw_len, never strlen: the body may hold NUL bytes. */
    size_t body_len = (size_t)cl;
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
    const char *p = query;
    while (*p != '\0' && req->query_count < MAX_QUERY_PARAMS) {
        const char *amp = strchr(p, '&');
        const size_t pair_len = amp != NULL ? (size_t)(amp - p) : strlen(p);
        if (pair_len > 0) {
            const char *eq = memchr(p, '=', pair_len);
            const size_t name_len = eq != NULL ? (size_t)(eq - p) : pair_len;
            const char *value = eq != NULL ? eq + 1 : p + pair_len;
            const size_t value_len = eq != NULL ? pair_len - name_len - 1 : 0;

            decode_bounded(req->query_names[req->query_count], sizeof(req->query_names[0]), p, name_len, 1);
            decode_bounded(req->query_values[req->query_count], sizeof(req->query_values[0]), value, value_len, 1);
            req->query_count++;
        }
        if (amp == NULL) {
            break;
        }
        p = amp + 1;
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

static void parse_headers_n(const char *block, const size_t len, Request *req) {
    req->header_count = 0;
    const char *p = block;
    const char *end = block + len;

    while (p < end && req->header_count < MAX_HEADERS) {
        const char *le = find_line_end(p, end);
        const char *colon = memchr(p, ':', (size_t)(le - p));
        if (colon != NULL) {
            const char *value = colon + 1;
            while (value < le && is_ows(*value)) {
                value++;
            }
            const char *value_end = le;
            while (value_end > value && is_ows(value_end[-1])) {
                value_end--;
            }
            copy_bounded(req->header_names[req->header_count], sizeof(req->header_names[0]), p,
                         (size_t)(colon - p));
            copy_bounded(req->header_values[req->header_count], sizeof(req->header_values[0]), value,
                         (size_t)(value_end - value));
            req->header_count++;
        }
        /* A line with no ':' is malformed and skipped. */
        if (le >= end) {
            break;
        }
        p = le + 2;
    }
}

void parse_headers(const char *header_block, Request *req) {
    parse_headers_n(header_block, strlen(header_block), req);
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
    if (cookie_header == NULL) {
        return;
    }

    const char *p = cookie_header;
    while (*p != '\0' && req->cookie_count < MAX_COOKIES) {
        while (*p == ' ' || *p == ';') {
            p++;
        }
        const char *semi = strchr(p, ';');
        const size_t pair_len = semi != NULL ? (size_t)(semi - p) : strlen(p);
        const char *eq = memchr(p, '=', pair_len);
        if (eq != NULL) {
            copy_bounded(req->cookie_names[req->cookie_count], sizeof(req->cookie_names[0]), p, (size_t)(eq - p));
            copy_bounded(req->cookie_values[req->cookie_count], sizeof(req->cookie_values[0]), eq + 1,
                         pair_len - (size_t)(eq - p) - 1);
            req->cookie_count++;
        }
        /* A pair with no '=' is malformed and skipped. */
        if (semi == NULL) {
            break;
        }
        p = semi + 1;
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

const char *status_text(const int status) {
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

/* ---- chunked request bodies ---- */

/* Pointer to the "\r" of the first CRLF in buf[0..len), or NULL. Length-bounded:
 * chunk data next to the framing line is opaque bytes and must never be scanned. */
static const char *find_crlf(const char *buf, const size_t len) {
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n') {
            return buf + i;
        }
    }
    return NULL;
}

/* "\r\n\r\n" ending a chunked body's trailer-part; found from the last-chunk line's own start. */
static const char *find_double_crlf(const char *buf, const size_t len) {
    return find_header_end(buf, len);
}

int chunked_body_scan(const char *body_start, const size_t available, const size_t max_decoded_len,
                      size_t *decoded_len_out) {
    size_t pos = 0;
    size_t decoded_len = 0;

    for (;;) {
        *decoded_len_out = decoded_len;
        if (pos >= available) {
            return 0;
        }

        const char *line_start = body_start + pos;
        const char *line_end = find_crlf(line_start, available - pos);
        if (line_end == NULL) {
            /* No CRLF yet: a size line this long is malformed, otherwise still arriving. */
            return (available - pos > MAX_CHUNK_SIZE_LINE_LEN) ? -1 : 0;
        }

        const size_t line_len = (size_t)(line_end - line_start);
        if (line_len == 0 || line_len > MAX_CHUNK_SIZE_LINE_LEN) {
            return -1;
        }

        /* "<hex-size>[;ext]": extensions are accepted and ignored. */
        char size_buf[MAX_CHUNK_SIZE_LINE_LEN + 1];
        memcpy(size_buf, line_start, line_len);
        size_buf[line_len] = '\0';
        char *ext = strchr(size_buf, ';');
        if (ext != NULL) {
            *ext = '\0';
        }
        if (size_buf[0] == '\0') {
            return -1;
        }

        char *endptr;
        const unsigned long chunk_size = strtoul(size_buf, &endptr, 16);
        if (*endptr != '\0') {
            return -1; /* non-hex characters in the size */
        }

        if (chunk_size == 0) {
            /* Last chunk: complete once the trailer-part's terminating blank line is here. */
            return find_double_crlf(line_start, available - pos) != NULL ? 1 : 0;
        }

        /* Checked against the *declared* size before its data arrives; decoded_len
         * never exceeds max_decoded_len, so the subtraction cannot underflow. */
        if (chunk_size > max_decoded_len - decoded_len) {
            return -2;
        }

        pos += line_len + 2;
        if (pos + chunk_size + 2 > available) {
            return 0; /* this chunk's data / trailing CRLF has not fully arrived */
        }
        if (body_start[pos + chunk_size] != '\r' || body_start[pos + chunk_size + 1] != '\n') {
            return -1; /* size does not match data: every later boundary would desync */
        }

        decoded_len += chunk_size;
        pos += chunk_size + 2;
    }
}

size_t chunked_body_decode(const char *body_start, const size_t available, char *out) {
    size_t pos = 0;
    size_t out_len = 0;

    for (;;) {
        const char *line_start = body_start + pos;
        const char *line_end = find_crlf(line_start, available - pos);
        const size_t line_len = (size_t)(line_end - line_start);

        char size_buf[MAX_CHUNK_SIZE_LINE_LEN + 1];
        memcpy(size_buf, line_start, line_len);
        size_buf[line_len] = '\0';
        char *ext = strchr(size_buf, ';');
        if (ext != NULL) {
            *ext = '\0';
        }
        const unsigned long chunk_size = strtoul(size_buf, NULL, 16);

        pos += line_len + 2;
        if (chunk_size == 0) {
            break; /* trailer header lines, if any, are discarded */
        }

        memcpy(out + out_len, body_start + pos, chunk_size);
        out_len += chunk_size;
        pos += chunk_size + 2;
    }

    return out_len;
}
