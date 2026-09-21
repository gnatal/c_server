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

int request_framing(const char *buf, const size_t len, size_t *header_len_out, int *chunked_out) {
    *header_len_out = 0;
    *chunked_out = 0;

    const char *method, *path;
    size_t method_len, path_len;
    int minor_version;
    struct phr_header headers[100];
    size_t num_headers = 100;

    int res = phr_parse_request(buf, len, &method, &method_len, &path, &path_len,
                                &minor_version, headers, &num_headers, 0);

    if (res == -2) return 0; // Incomplete
    if (res == -1) return -1; // Parse error

    *header_len_out = (size_t)res;

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


int request_is_complete(const char *buf, const size_t len) {
    size_t header_len;
    int chunked;
    const int content_length = request_framing(buf, len, &header_len, &chunked);
    if (header_len == 0) return 0;
    if (content_length < 0) return 1;

    const size_t body_have = len - header_len;
    if (chunked) {
        size_t decoded_len;
        return chunked_body_scan(buf + header_len, body_have, MAX_BODY_SIZE, &decoded_len) != 0;
    }
    return body_have >= (size_t)content_length;
}

/* ---- request parsing ---- */

int parse_http_request(const char *raw, const size_t raw_len, Request *req, Arena *arena) {
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

    const char *method, *path;
    size_t method_len, path_len;
    int minor_version;
    struct phr_header headers[MAX_HEADERS];
    size_t num_headers = MAX_HEADERS;

    int res = phr_parse_request(raw, raw_len, &method, &method_len, &path, &path_len,
                                &minor_version, headers, &num_headers, 0);

    if (res <= 0) return -1;

    if (method_len >= sizeof(req->method)) return -1;

    copy_bounded(req->method, sizeof(req->method), method, method_len);
    snprintf(req->version, sizeof(req->version), "HTTP/1.%d", minor_version);

    const char *qmark = memchr(path, '?', path_len);
    size_t p_len = qmark ? (size_t)(qmark - path) : path_len;
    if (p_len >= sizeof(req->path)) return -2;
    
    copy_bounded(req->path, sizeof(req->path), path, p_len);
    url_decode(req->path, req->path, sizeof(req->path), 0);
    
    if (qmark) {
        size_t q_len = path_len - p_len - 1;
        copy_bounded(req->query, sizeof(req->query), qmark + 1, q_len);
    }
    parse_query_string(req->query, req);

    for (size_t i = 0; i < num_headers; i++) {
        if (req->header_count < MAX_HEADERS) {
            copy_bounded(req->header_names[req->header_count], sizeof(req->header_names[0]), headers[i].name, headers[i].name_len);
            copy_bounded(req->header_values[req->header_count], sizeof(req->header_values[0]), headers[i].value, headers[i].value_len);
            req->header_count++;
        }
    }
    parse_cookies(req_get_header(req, "Cookie"), req);

    size_t header_len = (size_t)res;
    const char *body_start = raw + header_len;
    const size_t available = raw_len - header_len;

    int chunked = 0;
    int content_length = request_framing(raw, raw_len, &header_len, &chunked);
    
    if (content_length < 0) {
        req->content_length = content_length;
        return -1;
    }

    if (chunked) {
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
    
    req->content_length = content_length;
    
    size_t body_len = (size_t)content_length;
    if (body_len > available) body_len = available;
    req->body = arena_alloc(arena, body_len + 1);
    if (!req->body) return -1;
    memcpy(req->body, body_start, body_len);
    req->body[body_len] = '\0';
    return 0;
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
