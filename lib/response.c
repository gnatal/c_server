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

void res_status(Response *res, int status) {
    res->status = status;
}

void res_set_header(Response *res, const char *name, const char *value) {
    if (strcasecmp(name, "Content-Length") == 0 || strcasecmp(name, "Connection") == 0) {
        fprintf(stderr, "res_set_header: \"%s\" is managed by the response layer and cannot be overridden\n", name);
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

static void send_with_content_type(Response *res, const char *content_type, const char *body, size_t body_len) {
    char header[RESPONSE_HEADER_BUF_SIZE];
    Connection *conn = res->conn;

    /* A custom Content-Type (set via res_set_header) takes priority over
     * res_send/res_json's default, and must not be emitted twice below. */
    const char *custom_content_type = find_header(res, "Content-Type");
    if (custom_content_type != NULL) {
        content_type = custom_content_type;
    }

    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: %s\r\n",
        res->status, status_text(res->status), content_type, body_len,
        conn->keep_alive ? "keep-alive" : "close");

    int overflow = (n < 0 || (size_t)n >= sizeof(header));
    size_t offset = overflow ? sizeof(header) : (size_t)n;

    for (int i = 0; i < res->header_count && !overflow; i++) {
        if (strcasecmp(res->headers[i].name, "Content-Type") == 0) {
            continue;
        }
        n = snprintf(header + offset, sizeof(header) - offset,
            "%s: %s\r\n", res->headers[i].name, res->headers[i].value);
        if (n < 0 || (size_t)n >= sizeof(header) - offset) {
            overflow = 1;
        } else {
            offset += (size_t)n;
        }
    }

    /* One "Set-Cookie: ..." line per res_set_cookie() call - unlike the
     * headers[] loop above, this never overwrites/dedupes by name (see
     * res_set_cookie, app_types.h: Response.set_cookies). */
    for (int i = 0; i < res->set_cookie_count && !overflow; i++) {
        n = snprintf(header + offset, sizeof(header) - offset,
            "Set-Cookie: %s\r\n", res->set_cookies[i]);
        if (n < 0 || (size_t)n >= sizeof(header) - offset) {
            overflow = 1;
        } else {
            offset += (size_t)n;
        }
    }

    if (!overflow) {
        n = snprintf(header + offset, sizeof(header) - offset, "\r\n");
        if (n < 0 || (size_t)n >= sizeof(header) - offset) {
            overflow = 1;
        } else {
            offset += (size_t)n;
        }
    }

    if (overflow) {
        /* Headers can't fit the fixed buffer - drop the connection rather
         * than send a truncated/malformed response. */
        conn->out_len = 0;
        conn->out_sent = 0;
        conn->keep_alive = 0;
        return;
    }

    const size_t header_len = offset;
    /* Content-Length above is always computed from the full body - a HEAD
     * response must report the same length a GET would have (RFC 7231
     * 4.3.2), it just never actually sends those bytes. When body is NULL,
     * this is a file stream (headers only). */
    const size_t sent_body_len = (body != NULL && !res->is_head_request) ? body_len : 0;

    /*
     * Ownership: this buffer is handed to the event loop. On the keep-alive
     * path it's freed once fully written in flush_connection(); on any
     * error/close path it's freed in connection_close(). Exactly one of
     * those runs for every connection, so exactly one free matches this
     * malloc.
     */
    conn->out_buf = malloc(header_len + sent_body_len);
    if (conn->out_buf == NULL) {
        /* Out of memory: nothing safe to send back. Drop the connection
         * rather than write through a NULL pointer. */
        conn->out_len = 0;
        conn->out_sent = 0;
        conn->keep_alive = 0;
        return;
    }
    memcpy(conn->out_buf, header, header_len);
    if (sent_body_len > 0) {
        memcpy(conn->out_buf + header_len, body, sent_body_len);
    }
    conn->out_len = header_len + sent_body_len;
    conn->out_sent = 0;
    conn->out_cap = header_len + sent_body_len;
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
    static const CookieOptions defaults = { .max_age = -1, .path = NULL, .domain = NULL,
                                             .http_only = 0, .secure = 0, .same_site = COOKIE_SAMESITE_UNSET };
    if (options == NULL) {
        options = &defaults;
    }

    if (res->set_cookie_count >= MAX_RESPONSE_COOKIES) {
        fprintf(stderr, "res_set_cookie: MAX_RESPONSE_COOKIES exceeded\n");
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
    if (!overflow && options->max_age >= 0) {
        n = snprintf(dest + offset, cap - offset, "; Max-Age=%d", options->max_age);
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
    const CookieOptions options = { .max_age = 0, .path = path, .domain = NULL,
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
    if (conn->out_len + len > conn->out_cap) {
        size_t new_cap = (conn->out_cap == 0) ? 1024 : conn->out_cap * 2;
        while (new_cap < conn->out_len + len) {
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
    return 0;
}

static int commit_chunked_headers(Response *res) {
    char header[RESPONSE_HEADER_BUF_SIZE];
    Connection *conn = res->conn;
    if (conn == NULL) {
        return -1;
    }

    if (res->status == 0) {
        res->status = 200;
    }

    const char *content_type = "text/plain";
    const char *custom_content_type = find_header(res, "Content-Type");
    if (custom_content_type != NULL) {
        content_type = custom_content_type;
    }

    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: %s\r\n",
        res->status, status_text(res->status), content_type,
        conn->keep_alive ? "keep-alive" : "close");

    int overflow = (n < 0 || (size_t)n >= sizeof(header));
    size_t offset = overflow ? sizeof(header) : (size_t)n;

    for (int i = 0; i < res->header_count && !overflow; i++) {
        if (strcasecmp(res->headers[i].name, "Content-Type") == 0 ||
            strcasecmp(res->headers[i].name, "Transfer-Encoding") == 0 ||
            strcasecmp(res->headers[i].name, "Content-Length") == 0 ||
            strcasecmp(res->headers[i].name, "Connection") == 0) {
            continue;
        }
        n = snprintf(header + offset, sizeof(header) - offset,
            "%s: %s\r\n", res->headers[i].name, res->headers[i].value);
        if (n < 0 || (size_t)n >= sizeof(header) - offset) {
            overflow = 1;
        } else {
            offset += (size_t)n;
        }
    }

    for (int i = 0; i < res->set_cookie_count && !overflow; i++) {
        n = snprintf(header + offset, sizeof(header) - offset,
            "Set-Cookie: %s\r\n", res->set_cookies[i]);
        if (n < 0 || (size_t)n >= sizeof(header) - offset) {
            overflow = 1;
        } else {
            offset += (size_t)n;
        }
    }

    if (res->trailer_count > 0 && !overflow) {
        n = snprintf(header + offset, sizeof(header) - offset, "Trailer: ");
        if (n < 0 || (size_t)n >= sizeof(header) - offset) {
            overflow = 1;
        } else {
            offset += (size_t)n;
            for (int i = 0; i < res->trailer_count && !overflow; i++) {
                n = snprintf(header + offset, sizeof(header) - offset,
                    "%s%s", res->trailers[i].name,
                    (i + 1 < res->trailer_count) ? ", " : "\r\n");
                if (n < 0 || (size_t)n >= sizeof(header) - offset) {
                    overflow = 1;
                } else {
                    offset += (size_t)n;
                }
            }
        }
    }

    if (!overflow) {
        n = snprintf(header + offset, sizeof(header) - offset, "\r\n");
        if (n < 0 || (size_t)n >= sizeof(header) - offset) {
            overflow = 1;
        } else {
            offset += (size_t)n;
        }
    }

    if (overflow) {
        conn->out_len = 0;
        conn->out_sent = 0;
        conn->keep_alive = 0;
        return -1;
    }

    if (append_to_out_buf(conn, header, offset) != 0) {
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
