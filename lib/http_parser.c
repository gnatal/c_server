#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "http_parser.h"


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

/*
 * Percent-decodes src[0..src_len) into dst (dst_size bytes incl. NUL). dst may equal src: output is
 * never longer than input, so the write cursor never overtakes the read cursor. Returns 0 on success,
 * -1 if a decoded byte is NUL (S6: a raw NUL byte, or "%00", decoded into the middle of dst - dst is
 * still fully written and NUL-terminated in that case, but every C-string function downstream
 * (strlen, strcmp, the router, a handler's own strstr/strcmp on req->path) would silently see only the
 * bytes up to that NUL, treating "/style.css%00.png" as "/style.css" with no error and no indication
 * anything was cut. Callers must reject such input (400) rather than use dst). */
static int decode_bounded(char *dst, const size_t dst_size, const char *src, const size_t src_len,
                          const int decode_plus) {
    if (dst_size == 0) {
        return 0;
    }
    size_t out = 0;
    int has_embedded_nul = 0;
    for (size_t i = 0; i < src_len && out + 1 < dst_size; i++) {
        char c;
        if (src[i] == '%' && i + 2 < src_len && hex_value(src[i + 1]) >= 0 && hex_value(src[i + 2]) >= 0) {
            c = (char)((hex_value(src[i + 1]) << 4) | hex_value(src[i + 2]));
            i += 2;
        } else if (src[i] == '+' && decode_plus) {
            c = ' ';
        } else {
            c = src[i];
        }
        if (c == '\0') {
            has_embedded_nul = 1;
        }
        dst[out++] = c;
    }
    dst[out] = '\0';
    return has_embedded_nul ? -1 : 0;
}

int url_decode(const char *src, char *dst, const size_t dst_size, const int decode_plus) {
    return decode_bounded(dst, dst_size, src, strlen(src), decode_plus);
}


#include "vendor/picohttpparser/picohttpparser.h"

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

/* Shared by parse_request_head: derives the Content-Length / Transfer-Encoding framing verdict from an
 * already-tokenized header array (P2 - this used to be inlined once in request_framing and duplicated,
 * differently, a second time inside parse_http_request; now there is exactly one copy). */
static int compute_content_length_and_chunked(const struct phr_header *headers, const size_t num_headers,
                                               int *chunked_out) {
    *chunked_out = 0;
    int content_length = 0;
    int has_cl = 0;

    for (size_t i = 0; i < num_headers; i++) {
        if (headers[i].name_len == 14 && strncasecmp(headers[i].name, "Content-Length", 14) == 0) {
            long long acc = 0;
            const char *v = headers[i].value;
            size_t v_len = headers[i].value_len;
            size_t j = 0;
            while (j < v_len && (v[j] == ' ' || v[j] == '\t')) j++;
            if (j < v_len && v[j] == '-') {
                has_cl = 1; content_length = -1;
            } else if (j == v_len || v[j] < '0' || v[j] > '9') {
                has_cl = 1; content_length = -1;
            } else {
                while (j < v_len && v[j] >= '0' && v[j] <= '9') {
                    if (acc <= MAX_BODY_SIZE) acc = acc * 10 + (v[j] - '0');
                    j++;
                }
                while (j < v_len && (v[j] == ' ' || v[j] == '\t')) j++;
                int val = (j != v_len) ? -1 : (acc > MAX_BODY_SIZE ? -2 : (int)acc);
                if (has_cl && val != content_length) content_length = -1;
                else if (!has_cl) content_length = val;
                has_cl = 1;
            }
        } else if (headers[i].name_len == 17 && strncasecmp(headers[i].name, "Transfer-Encoding", 17) == 0) {
            const char *q = headers[i].value;
            size_t q_len = headers[i].value_len;
            for (size_t k = 0; k + 7 <= q_len; k++) {
                if (strncasecmp(q + k, "chunked", 7) == 0) {
                    *chunked_out = 1;
                    break;
                }
            }
        }
    }

    if (*chunked_out && has_cl) return -1;
    return content_length;
}

int parse_request_head(const char *buf, const size_t len, ParsedHead *head) {
    head->header_len = 0;
    head->method = NULL;
    head->method_len = 0;
    head->path = NULL;
    head->path_len = 0;
    head->minor_version = 0;
    head->content_length = 0;
    head->chunked = 0;
    head->num_headers = MAX_FRAMING_HEADERS;

    const int res = phr_parse_request(buf, len, &head->method, &head->method_len, &head->path, &head->path_len,
                                      &head->minor_version, head->headers, &head->num_headers, 0);

    if (res == -2) return 0; // Incomplete: header_len stays 0
    if (res == -1) {
        /* Malformed request line: header_len stays 0 too, distinguished from "incomplete" only by this
         * return value - mirrors request_framing's pre-existing contract, including its S8 known gap
         * (request_head_is_complete checks header_len == 0 before this). */
        head->content_length = -1;
        return -1;
    }

    head->header_len = (size_t)res;
    head->content_length = compute_content_length_and_chunked(head->headers, head->num_headers, &head->chunked);
    return head->content_length;
}

int request_framing(const char *buf, const size_t len, size_t *header_len_out, int *chunked_out,
                    const char **path_out, size_t *path_len_out) {
    ParsedHead head;
    const int result = parse_request_head(buf, len, &head);
    *header_len_out = head.header_len;
    *chunked_out = head.chunked;
    if (path_out != NULL) *path_out = head.path;
    if (path_len_out != NULL) *path_len_out = head.path_len;
    return result;
}








static const char *find_line_end(const char *buf, const char *end) {
    const char *p = memchr(buf, '\r', (size_t)(end - buf));
    if (p != NULL && p + 1 < end && *(p + 1) == '\n') {
        return p;
    }
    return end;
}

static int scan_framing(const char *block, const size_t len, int *has_cl_out, int *chunked_out) {
    const char *p = block;
    const char *end = block + len;
    int result = 0;
    int has_cl = 0;
    int chunked = 0;

    while (p < end) {
        const char *le = find_line_end(p, end);
        if (le == p) break;
        const char *colon = memchr(p, ':', (size_t)(le - p));
        if (colon != NULL) {
            const size_t name_len = (size_t)(colon - p);
            const char *v = colon + 1;
            while (v < le && is_ows(*v)) v++;

            if (name_len == 14 && strncasecmp(p, "Content-Length", 14) == 0) {
                int value = 0;
                if (v < le && *v == '-') value = -1;
                else if (v == le || *v < '0' || *v > '9') value = -1;
                else {
                    long long acc = 0;
                    while (v < le && *v >= '0' && *v <= '9') {
                        if (acc <= MAX_BODY_SIZE) acc = acc * 10 + (*v - '0');
                        v++;
                    }
                    while (v < le && is_ows(*v)) v++;
                    if (v != le) value = -1;
                    else if (acc > MAX_BODY_SIZE) value = -2;
                    else value = (int)acc;
                }
                if (has_cl && value != result) result = -1;
                else if (!has_cl) result = value;
                has_cl = 1;
            } else if (name_len == 17 && strncasecmp(p, "Transfer-Encoding", 17) == 0) {
                for (const char *q = v; (size_t)(le - q) >= 7; q++) {
                    if (strncasecmp(q, "chunked", 7) == 0) { chunked = 1; break; }
                }
            }
        }
        if (le >= end) break;
        p = le + 2;
    }
    if (has_cl_out != NULL) *has_cl_out = has_cl;
    if (chunked_out != NULL) *chunked_out = chunked;
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



int request_wants_close(const Request *req) {
    const char *connection = req_get_header(req, "Connection");
    if (connection != NULL) {
        if (list_has_token(connection, "close")) return 1;
        if (list_has_token(connection, "keep-alive")) return 0;
    }
    return strcmp(req->version, "HTTP/1.1") != 0;
}


int request_head_is_complete(const ParsedHead *head, const char *buf, const size_t len) {
    if (head->header_len == 0) return 0;
    if (head->content_length < 0) return 1;

    const size_t body_have = len - head->header_len;
    if (head->chunked) {
        size_t decoded_len;
        return chunked_body_scan(buf + head->header_len, body_have, MAX_BODY_SIZE, &decoded_len) != 0;
    }
    return body_have >= (size_t)head->content_length;
}

int request_is_complete(const char *buf, const size_t len) {
    ParsedHead head;
    parse_request_head(buf, len, &head);
    return request_head_is_complete(&head, buf, len);
}

/* ---- request parsing ---- */

static void reset_request(Request *req) {
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
}

int parse_http_request_from_head(const char *raw, const size_t raw_len, const ParsedHead *head,
                                 Request *req, Arena *arena) {
    reset_request(req);

    /* S5/"33rd header": parse_request_head's own phr_parse_request runs with a larger header-array
     * capacity (MAX_FRAMING_HEADERS) than the engine stores (MAX_HEADERS) precisely so a request with
     * more headers than the engine keeps is diagnosed here as malformed, not mis-reported as
     * "incomplete" by request_head_is_complete's header_len == 0 check (S8's known gap - this must not
     * widen it). Checked before anything else is copied, so a rejected request leaves req exactly as
     * reset above, same as when phr_parse_request itself used to fail outright on this (its own
     * capacity was MAX_HEADERS before this split). */
    if (head->num_headers > MAX_HEADERS) {
        return -1;
    }

    if (head->method_len >= sizeof(req->method)) return -1;

    copy_bounded(req->method, sizeof(req->method), head->method, head->method_len);
    snprintf(req->version, sizeof(req->version), "HTTP/1.%d", head->minor_version);

    const char *qmark = memchr(head->path, '?', head->path_len);
    size_t p_len = qmark ? (size_t)(qmark - head->path) : head->path_len;
    if (p_len >= sizeof(req->path)) return -2;

    copy_bounded(req->path, sizeof(req->path), head->path, p_len);
    if (url_decode(req->path, req->path, sizeof(req->path), 0) != 0) {
        /* S6: "%00" (or a raw NUL byte) decoded into the middle of the path - a filter that checks
         * req->path's suffix/extension before using it (e.g. a static-file extension check) would see
         * only the bytes up to the NUL, so "/style.css%00.png" would look like "/style.css". Reject
         * outright (400) rather than route on a silently truncated path. */
        return -4;
    }

    if (qmark) {
        size_t q_len = head->path_len - p_len - 1;
        copy_bounded(req->query, sizeof(req->query), qmark + 1, q_len);
    }
    if (parse_query_string(req->query, req) != 0) {
        return -4;
    }

    for (size_t i = 0; i < head->num_headers; i++) {
        if (req->header_count < MAX_HEADERS) {
            /* S5: a name or value that would not fit is rejected (431), never silently truncated -
             * copy_bounded used to cut a value at 255 bytes with no error, so a Bearer JWT or a long
             * cookie header compared unequal to itself with nothing in the response explaining why. */
            if (head->headers[i].name_len >= sizeof(req->header_names[0]) ||
                head->headers[i].value_len >= sizeof(req->header_values[0])) {
                return -3;
            }
            copy_bounded(req->header_names[req->header_count], sizeof(req->header_names[0]),
                        head->headers[i].name, head->headers[i].name_len);
            copy_bounded(req->header_values[req->header_count], sizeof(req->header_values[0]),
                        head->headers[i].value, head->headers[i].value_len);
            req->header_count++;
        }
    }
    parse_cookies(req_get_header(req, "Cookie"), req);

    if (head->content_length < 0) {
        req->content_length = head->content_length;
        return -1;
    }

    const char *body_start = raw + head->header_len;
    const size_t available = raw_len - head->header_len;

    if (head->chunked) {
        size_t decoded_len = 0;
        int scan = chunked_body_scan(body_start, available, MAX_BODY_SIZE, &decoded_len);
        if (scan == -2) {
            req->content_length = -2;
            return -1;
        }
        if (scan != 1) return -1;
        req->body = arena_alloc(arena, decoded_len + 1);
        if (!req->body) return -1;
        size_t written = chunked_body_decode(body_start, available, req->body);
        req->body[written] = '\0';
        req->content_length = (int)written;
        return 0;
    }

    req->content_length = head->content_length;

    size_t body_len = (size_t)head->content_length;
    if (body_len > available) body_len = available;
    req->body = arena_alloc(arena, body_len + 1);
    if (!req->body) return -1;
    memcpy(req->body, body_start, body_len);
    req->body[body_len] = '\0';
    return 0;
}

int parse_http_request(const char *raw, const size_t raw_len, Request *req, Arena *arena) {
    ParsedHead head;
    parse_request_head(raw, raw_len, &head);
    if (head.header_len == 0) {
        reset_request(req);
        return -1;
    }
    return parse_http_request_from_head(raw, raw_len, &head, req, arena);
}

void parse_headers(const char *header_block, Request *req) {
    struct phr_header headers[MAX_HEADERS];
    size_t num_headers = MAX_HEADERS;
    int res = phr_parse_headers(header_block, strlen(header_block), headers, &num_headers, 0);
    if (res == -1) return;
    
    req->header_count = 0;
    for (size_t i = 0; i < num_headers; i++) {
        if (req->header_count < MAX_HEADERS) {
            copy_bounded(req->header_names[req->header_count], sizeof(req->header_names[0]), headers[i].name, headers[i].name_len);
            copy_bounded(req->header_values[req->header_count], sizeof(req->header_values[0]), headers[i].value, headers[i].value_len);
            req->header_count++;
        }
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
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n') {
            return buf + i;
        }
    }
    return NULL;
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
int parse_query_string(const char *query, Request *req) {
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

            const int name_ok = decode_bounded(req->query_names[req->query_count], sizeof(req->query_names[0]), p, name_len, 1) == 0;
            const int value_ok = decode_bounded(req->query_values[req->query_count], sizeof(req->query_values[0]), value, value_len, 1) == 0;
            req->query_count++;
            if (!name_ok || !value_ok) {
                /* S6: a %00 (or a raw NUL byte) decoded into this pair - reject the whole request (400)
                 * rather than let req_get_query silently hand back a truncated name or value. */
                return -1;
            }
        }
        if (amp == NULL) {
            break;
        }
        p = amp + 1;
    }
    return 0;
}

const char *req_get_query(const Request *req, const char *name) {
    for (int i = 0; i < req->query_count; i++) {
        if (strcmp(req->query_names[i], name) == 0) {
            return req->query_values[i];
        }
    }
    return NULL;
}
