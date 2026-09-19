#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "response.h"
#include "http_parser.h"

/*
 * Large enough for the status line, the three built-in headers, and up to
 * MAX_RESPONSE_HEADERS custom headers (bounded to the name/value sizes in
 * ResponseHeader) with room to spare - a fixed cap in the same spirit as the
 * other hard limits in this engine (BUF_SIZE, MAX_ROUTES, ...) rather than a
 * dynamically grown buffer.
 */
#define RESPONSE_HEADER_BUF_SIZE 8192

/*
 * Header/cookie text is written into the response verbatim, and handlers routinely pass request data
 * (req_get_query decodes %0d%0a into CR LF). Any control character would let a client inject headers or
 * a second response (HTTP response splitting), so such text is refused here rather than trusted to callers.
 * Bytes >= 0x80 (UTF-8) pass. HTAB is allowed in values only. `extra_forbidden` adds delimiter characters.
 */
static int text_is_safe(const char *text, const int allow_htab, const char *extra_forbidden) {
    for (const unsigned char *c = (const unsigned char *)text; *c != '\0'; c++) {
        if ((*c < 0x20 && !(allow_htab && *c == '\t')) || *c == 0x7f) {
            return 0;
        }
        if (extra_forbidden != NULL && strchr(extra_forbidden, *c) != NULL) {
            return 0;
        }
    }
    return 1;
}

void res_init(Response *res, Connection *conn) {
    /* Only the scalars and *_count fields are initialized: the header/cookie/
     * trailer arrays are never read past their count, so zeroing the whole
     * ~16 KB struct per request would be wasted work. */
    res->conn = conn;
    res->status = 200;
    res->header_count = 0;
    res->set_cookie_count = 0;
    res->trailer_count = 0;
    res->is_head_request = 0;
    res->is_chunked = 0;
    res->headers_sent = 0;
    res->stream_ended = 0;
}

void res_status(Response *res, int status) {
    res->status = status;
}

void res_set_header(Response *res, const char *name, const char *value) {
    if (strcasecmp(name, "Content-Length") == 0 || strcasecmp(name, "Connection") == 0) {
        fprintf(stderr, "res_set_header: \"%s\" is managed by the response layer and cannot be overridden\n", name);
        return;
    }

    if (name[0] == '\0' || !text_is_safe(name, 0, ":") || !text_is_safe(value, 1, NULL)) {
        fprintf(stderr, "res_set_header: header \"%s\" has an empty name or a control character, dropped\n", name);
        return;
    }

    for (int i = 0; i < res->header_count; i++) {
        if (strcasecmp(res->headers[i].name, name) == 0) {
            strncpy(res->headers[i].value, value, sizeof(res->headers[i].value) - 1);
            res->headers[i].value[sizeof(res->headers[i].value) - 1] = '\0';
            return;
        }
    }

    if (res->header_count >= MAX_RESPONSE_HEADERS) {
        fprintf(stderr, "res_set_header: MAX_RESPONSE_HEADERS exceeded\n");
        return;
    }

    ResponseHeader *header = &res->headers[res->header_count++];
    strncpy(header->name, name, sizeof(header->name) - 1);
    header->name[sizeof(header->name) - 1] = '\0';
    strncpy(header->value, value, sizeof(header->value) - 1);
    header->value[sizeof(header->value) - 1] = '\0';
}

static const char *find_header(const Response *res, const char *name) {
    for (int i = 0; i < res->header_count; i++) {
        if (strcasecmp(res->headers[i].name, name) == 0) {
            return res->headers[i].value;
        }
    }
    return NULL;
}

/* ---- response head assembly: bounded appends into a caller-owned buffer ---- */

/* Appends n bytes at *off; -1 (and no write) if they would not fit in cap. */
static int put_bytes(char *buf, const size_t cap, size_t *off, const char *s, const size_t n) {
    if (n > cap - *off) {
        return -1;
    }
    memcpy(buf + *off, s, n);
    *off += n;
    return 0;
}

static int put_str(char *buf, const size_t cap, size_t *off, const char *s) {
    return put_bytes(buf, cap, off, s, strlen(s));
}

static int put_uint(char *buf, const size_t cap, size_t *off, size_t value) {
    char digits[24];
    size_t i = sizeof(digits);
    do {
        digits[--i] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);
    return put_bytes(buf, cap, off, digits + i, sizeof(digits) - i);
}

/*
 * Writes "HTTP/1.1 <status> <reason>\r\nContent-Type: ..\r\n" + framing header
 * (Content-Length, or Transfer-Encoding: chunked when body_len == CHUNKED_BODY)
 * + "Connection: ..\r\n" + custom headers + Set-Cookie lines + (Trailer: names)
 * + blank line into buf. Returns the head length, or 0 if it does not fit.
 * A custom Content-Type (res_set_header) replaces `content_type` and is emitted once.
 */
#define CHUNKED_BODY ((size_t)-1)

static size_t build_response_head(const Response *res, const char *content_type, const size_t body_len,
                                  char *buf, const size_t cap) {
    size_t off = 0;
    const char *custom_content_type = find_header(res, "Content-Type");
    if (custom_content_type != NULL) {
        content_type = custom_content_type;
    }

    int bad = put_str(buf, cap, &off, "HTTP/1.1 ");
    bad |= put_uint(buf, cap, &off, (size_t)res->status);
    bad |= put_str(buf, cap, &off, " ");
    bad |= put_str(buf, cap, &off, status_text(res->status));
    bad |= put_str(buf, cap, &off, "\r\nContent-Type: ");
    bad |= put_str(buf, cap, &off, content_type);
    if (body_len == CHUNKED_BODY) {
        bad |= put_str(buf, cap, &off, "\r\nTransfer-Encoding: chunked");
    } else {
        bad |= put_str(buf, cap, &off, "\r\nContent-Length: ");
        bad |= put_uint(buf, cap, &off, body_len);
    }
    bad |= put_str(buf, cap, &off, res->conn->keep_alive ? "\r\nConnection: keep-alive\r\n" : "\r\nConnection: close\r\n");

    for (int i = 0; i < res->header_count && !bad; i++) {
        const char *name = res->headers[i].name;
        if (strcasecmp(name, "Content-Type") == 0 ||
            (body_len == CHUNKED_BODY && (strcasecmp(name, "Transfer-Encoding") == 0 ||
                                          strcasecmp(name, "Content-Length") == 0 ||
                                          strcasecmp(name, "Connection") == 0))) {
            continue;
        }
        bad |= put_str(buf, cap, &off, name);
        bad |= put_str(buf, cap, &off, ": ");
        bad |= put_str(buf, cap, &off, res->headers[i].value);
        bad |= put_str(buf, cap, &off, "\r\n");
    }

    /* One Set-Cookie line per res_set_cookie call: never deduplicated by name. */
    for (int i = 0; i < res->set_cookie_count && !bad; i++) {
        bad |= put_str(buf, cap, &off, "Set-Cookie: ");
        bad |= put_str(buf, cap, &off, res->set_cookies[i]);
        bad |= put_str(buf, cap, &off, "\r\n");
    }

    if (body_len == CHUNKED_BODY && res->trailer_count > 0 && !bad) {
        bad |= put_str(buf, cap, &off, "Trailer: ");
        for (int i = 0; i < res->trailer_count && !bad; i++) {
            bad |= put_str(buf, cap, &off, res->trailers[i].name);
            bad |= put_str(buf, cap, &off, (i + 1 < res->trailer_count) ? ", " : "\r\n");
        }
    }

    bad |= put_str(buf, cap, &off, "\r\n");
    return bad ? 0 : off;
}

/* Drops the connection: used when a response cannot be built (head too big, out of memory). */
static void abort_response(Connection *conn) {
    conn->out_len = 0;
    conn->out_sent = 0;
    conn->keep_alive = 0;
}

static void send_with_content_type(Response *res, const char *content_type, const char *body, const size_t body_len) {
    char head[RESPONSE_HEADER_BUF_SIZE];
    Connection *conn = res->conn;

    if (conn->file_fd >= 0) {
        close(conn->file_fd);
        conn->file_fd = -1;
        conn->file_remaining = 0;
    }

    const size_t head_len = build_response_head(res, content_type, body_len, head, sizeof(head));
    if (head_len == 0) {
        free(conn->out_buf);
        conn->out_buf = NULL;
        conn->out_cap = 0;
        abort_response(conn); /* headers do not fit: drop rather than send a malformed response */
        return;
    }

    /* Content-Length is always the full body's: a HEAD response reports what GET would
     * (RFC 7231 4.3.2) but sends no body bytes. body == NULL means a file stream (head only). */
    const size_t sent_body_len = (body != NULL && !res->is_head_request) ? body_len : 0;

    /* A second res_send/res_json in the same request replaces the first response (last wins);
     * without this the earlier buffer would leak. */
    free(conn->out_buf);

    /* Ownership: handed to the event loop. Freed exactly once, by flush_connection() when a
     * keep-alive response finishes or by connection_close() on any error/close path. */
    conn->out_buf = malloc(head_len + sent_body_len + 1);
    if (conn->out_buf == NULL) {
        conn->out_cap = 0;
        abort_response(conn);
        return;
    }
    memcpy(conn->out_buf, head, head_len);
    if (sent_body_len > 0) {
        memcpy(conn->out_buf + head_len, body, sent_body_len);
    }
    conn->out_len = head_len + sent_body_len;
    conn->out_buf[conn->out_len] = '\0'; /* not sent; lets tests strstr/strcmp the response safely */
    conn->out_sent = 0;
    conn->out_cap = head_len + sent_body_len + 1;
}

void res_send(Response *res, const char *body) {
    send_with_content_type(res, "text/plain", body, strlen(body));
}

void res_json(Response *res, const char *body) {
    send_with_content_type(res, "application/json", body, strlen(body));
}

void res_send_bytes(Response *res, const char *content_type, const unsigned char *data, size_t len) {
    send_with_content_type(res, content_type, (const char *)data, len);
}

void res_redirect(Response *res, int status, const char *location) {
    char body[300];

    if (!text_is_safe(location, 1, NULL)) {
        /* A Location with CR/LF is a response-splitting attempt: refuse instead of sending a broken redirect. */
        res_status(res, 500);
        res_send(res, "invalid redirect target");
        return;
    }
    res_status(res, status != 0 ? status : 302);
    res_set_header(res, "Location", location);
    snprintf(body, sizeof(body), "Redirecting to %s", location);
    res_send(res, body);
}

static const char *same_site_name(CookieSameSite same_site) {
    switch (same_site) {
        case COOKIE_SAMESITE_STRICT: return "Strict";
        case COOKIE_SAMESITE_LAX:    return "Lax";
        case COOKIE_SAMESITE_NONE:   return "None";
        default:                     return NULL;
    }
}

void res_set_cookie(Response *res, const char *name, const char *value, const CookieOptions *options) {
    static const CookieOptions defaults = { .max_age = 0, .path = NULL, .domain = NULL,
                                             .http_only = 0, .secure = 0, .same_site = COOKIE_SAMESITE_UNSET };
    if (options == NULL) {
        options = &defaults;
    }

    if (res->set_cookie_count >= MAX_RESPONSE_COOKIES) {
        fprintf(stderr, "res_set_cookie: MAX_RESPONSE_COOKIES exceeded\n");
        return;
    }

    if (name[0] == '\0' || !text_is_safe(name, 0, ";= ,") || !text_is_safe(value, 0, ";") ||
        (options->path != NULL && !text_is_safe(options->path, 0, ";")) ||
        (options->domain != NULL && !text_is_safe(options->domain, 0, ";"))) {
        fprintf(stderr, "res_set_cookie: cookie \"%s\" has a control character or delimiter in a field, dropped\n", name);
        return;
    }

    char *dest = res->set_cookies[res->set_cookie_count];
    const size_t cap = sizeof(res->set_cookies[0]);
    const char *path = options->path != NULL ? options->path : "/";

    int n = snprintf(dest, cap, "%s=%s; Path=%s", name, value, path);
    int overflow = (n < 0 || (size_t)n >= cap);
    size_t offset = overflow ? cap : (size_t)n;

    if (!overflow && options->domain != NULL) {
        n = snprintf(dest + offset, cap - offset, "; Domain=%s", options->domain);
        overflow = (n < 0 || (size_t)n >= cap - offset);
        if (!overflow) {
            offset += (size_t)n;
        }
    }
    if (!overflow && options->max_age != 0) {
        /* > 0: lifetime in seconds; < 0: expire now (sent as Max-Age=0); 0: session cookie, omitted */
        n = snprintf(dest + offset, cap - offset, "; Max-Age=%d", options->max_age > 0 ? options->max_age : 0);
        overflow = (n < 0 || (size_t)n >= cap - offset);
        if (!overflow) {
            offset += (size_t)n;
        }
    }
    if (!overflow && options->http_only) {
        n = snprintf(dest + offset, cap - offset, "; HttpOnly");
        overflow = (n < 0 || (size_t)n >= cap - offset);
        if (!overflow) {
            offset += (size_t)n;
        }
    }
    if (!overflow && options->secure) {
        n = snprintf(dest + offset, cap - offset, "; Secure");
        overflow = (n < 0 || (size_t)n >= cap - offset);
        if (!overflow) {
            offset += (size_t)n;
        }
    }
    if (!overflow && options->same_site != COOKIE_SAMESITE_UNSET) {
        n = snprintf(dest + offset, cap - offset, "; SameSite=%s", same_site_name(options->same_site));
        overflow = (n < 0 || (size_t)n >= cap - offset);
    }

    if (overflow) {
        fprintf(stderr, "res_set_cookie: cookie \"%s\" exceeds MAX_SET_COOKIE_LEN, dropped\n", name);
        return;
    }

    res->set_cookie_count++;
}

void res_clear_cookie(Response *res, const char *name, const char *path) {
    const CookieOptions options = { .max_age = -1, .path = path, .domain = NULL,
                                     .http_only = 0, .secure = 0, .same_site = COOKIE_SAMESITE_UNSET };
    res_set_cookie(res, name, "", &options);
}

static int append_to_out_buf(Connection *conn, const void *data, size_t len) {
    if (conn == NULL || data == NULL || len == 0) {
        return 0;
    }
    if (conn->out_len + len > (size_t)MAX_BODY_SIZE + RESPONSE_HEADER_BUF_SIZE) {
        fprintf(stderr, "append_to_out_buf: response exceeds MAX_BODY_SIZE cap\n");
        return -1;
    }
    if (conn->out_len + len + 1 > conn->out_cap) { /* +1: trailing NUL, see send_with_content_type */
        size_t new_cap = (conn->out_cap == 0) ? 1024 : conn->out_cap * 2;
        while (new_cap < conn->out_len + len + 1) {
            new_cap *= 2;
        }
        char *grown = realloc(conn->out_buf, new_cap);
        if (grown == NULL) {
            return -1;
        }
        conn->out_buf = grown;
        conn->out_cap = new_cap;
    }
    memcpy(conn->out_buf + conn->out_len, data, len);
    conn->out_len += len;
    conn->out_buf[conn->out_len] = '\0';
    return 0;
}

static int commit_chunked_headers(Response *res) {
    Connection *conn = res->conn;
    if (conn == NULL) {
        return -1;
    }
    if (conn->file_fd >= 0) {
        close(conn->file_fd);
        conn->file_fd = -1;
        conn->file_remaining = 0;
    }
    if (res->status == 0) {
        res->status = 200;
    }

    char head[RESPONSE_HEADER_BUF_SIZE];
    const size_t head_len = build_response_head(res, "text/plain", CHUNKED_BODY, head, sizeof(head));
    if (head_len == 0) {
        free(conn->out_buf);
        conn->out_buf = NULL;
        conn->out_cap = 0;
        abort_response(conn);
        return -1;
    }
    if (append_to_out_buf(conn, head, head_len) != 0) {
        return -1;
    }
    res->headers_sent = 1;
    res->is_chunked = 1;
    return 0;
}

void res_write(Response *res, const char *data, size_t len) {
    if (res == NULL || res->conn == NULL || res->stream_ended) {
        return;
    }
    if (!res->headers_sent) {
        if (commit_chunked_headers(res) != 0) {
            return;
        }
    }
    if (res->is_head_request) {
        return;
    }
    if (len == 0 && data == NULL) {
        return;
    }

    char chunk_hdr[32];
    int n = snprintf(chunk_hdr, sizeof(chunk_hdr), "%zx\r\n", len);
    if (n <= 0) {
        return;
    }
    if (append_to_out_buf(res->conn, chunk_hdr, (size_t)n) != 0) {
        return;
    }
    if (len > 0 && data != NULL) {
        if (append_to_out_buf(res->conn, data, len) != 0) {
            return;
        }
    }
    append_to_out_buf(res->conn, "\r\n", 2);
}

void res_set_trailer(Response *res, const char *name, const char *value) {
    if (res == NULL || name == NULL || value == NULL) {
        return;
    }
    if (strcasecmp(name, "Transfer-Encoding") == 0 ||
        strcasecmp(name, "Content-Length") == 0 ||
        strcasecmp(name, "Trailer") == 0) {
        fprintf(stderr, "res_set_trailer: \"%s\" is not permitted in chunked trailer\n", name);
        return;
    }

    if (name[0] == '\0' || !text_is_safe(name, 0, ":") || !text_is_safe(value, 1, NULL)) {
        fprintf(stderr, "res_set_trailer: trailer \"%s\" has an empty name or a control character, dropped\n", name);
        return;
    }

    for (int i = 0; i < res->trailer_count; i++) {
        if (strcasecmp(res->trailers[i].name, name) == 0) {
            strncpy(res->trailers[i].value, value, sizeof(res->trailers[i].value) - 1);
            res->trailers[i].value[sizeof(res->trailers[i].value) - 1] = '\0';
            return;
        }
    }

    if (res->trailer_count >= MAX_RESPONSE_TRAILERS) {
        fprintf(stderr, "res_set_trailer: MAX_RESPONSE_TRAILERS exceeded\n");
        return;
    }

    ResponseHeader *tr = &res->trailers[res->trailer_count++];
    strncpy(tr->name, name, sizeof(tr->name) - 1);
    tr->name[sizeof(tr->name) - 1] = '\0';
    strncpy(tr->value, value, sizeof(tr->value) - 1);
    tr->value[sizeof(tr->value) - 1] = '\0';
}

void res_end(Response *res) {
    if (res == NULL || res->conn == NULL || res->stream_ended) {
        return;
    }
    if (!res->headers_sent) {
        if (commit_chunked_headers(res) != 0) {
            return;
        }
    }
    res->stream_ended = 1;
    if (res->is_head_request) {
        return;
    }
    if (append_to_out_buf(res->conn, "0\r\n", 3) != 0) {
        return;
    }
    for (int i = 0; i < res->trailer_count; i++) {
        char tr_line[384];
        int n = snprintf(tr_line, sizeof(tr_line), "%s: %s\r\n",
                         res->trailers[i].name, res->trailers[i].value);
        if (n > 0) {
            append_to_out_buf(res->conn, tr_line, (size_t)n);
        }
    }
    append_to_out_buf(res->conn, "\r\n", 2);
}

int res_send_file(Response *res, const char *content_type, const char *filepath) {
    if (res == NULL || res->conn == NULL || filepath == NULL || res->headers_sent) {
        return -1;
    }
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return -1;
    }
    if (res->status == 0) {
        res->status = 200;
    }

    send_with_content_type(res, content_type, NULL, (size_t)st.st_size);
    if (res->conn->out_buf == NULL) {
        close(fd);
        res->conn->file_fd = -1;
        res->conn->file_remaining = 0;
        return -1;
    }
    res->headers_sent = 1;

    if (res->is_head_request || st.st_size == 0) {
        close(fd);
        res->conn->file_fd = -1;
        res->conn->file_remaining = 0;
    } else {
        res->conn->file_fd = fd;
        res->conn->file_remaining = (size_t)st.st_size;
    }
    return 0;
}
