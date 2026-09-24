#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include "response.h"
#include "http_parser.h"

/*
 * Cap on the whole response head (status line, built-in headers, custom headers, Set-Cookie lines).
 * Header values have no per-value cap (they live in the arena), so this is the one bound: a head that
 * does not fit is never sent shortened - the connection is dropped and logged.
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

/*
 * Shared by res_set_header and res_set_trailer (`what` names the caller in log lines). Same name
 * (case-insensitive) overwrites. The value is copied whole into the request arena - never shortened; a
 * name longer than 63 chars, a full table, or an arena out of space drops the entry (logged). An
 * overwritten value's old copy stays in the arena until its reset.
 */
static void set_named_value(Response *res, ResponseHeader *table, int *count, const int max,
                            const char *name, const char *value, const char *what) {
    const size_t name_len = strlen(name);
    if (name_len >= sizeof(table[0].name)) {
        fprintf(stderr, "res_set_%s: %s name \"%.63s...\" is longer than %zu chars, dropped\n", what, what, name,
                sizeof(table[0].name) - 1);
        return;
    }

    ResponseHeader *entry = NULL;
    for (int i = 0; i < *count; i++) {
        if (strcasecmp(table[i].name, name) == 0) {
            entry = &table[i];
            break;
        }
    }
    if (entry == NULL && *count >= max) {
        fprintf(stderr, "res_set_%s: MAX_RESPONSE_%sS exceeded\n", what, strcmp(what, "header") == 0 ? "HEADER" : "TRAILER");
        return;
    }

    const size_t value_len = strlen(value);
    char *copy = (res->conn != NULL && res->conn->arena != NULL) ? arena_alloc(res->conn->arena, value_len + 1) : NULL;
    if (copy == NULL) {
        fprintf(stderr, "res_set_%s: no arena space for %s \"%s\" (%zu bytes), dropped\n", what, what, name, value_len);
        return;
    }
    memcpy(copy, value, value_len + 1);

    if (entry == NULL) {
        entry = &table[(*count)++];
        memcpy(entry->name, name, name_len + 1);
    }
    entry->value = copy;
    entry->value_len = value_len;
}

void res_set_header(Response *res, const char *name, const char *value) {
    if (strcasecmp(name, "Content-Length") == 0 || strcasecmp(name, "Connection") == 0 ||
        strcasecmp(name, "Date") == 0) {
        fprintf(stderr, "res_set_header: \"%s\" is managed by the response layer and cannot be overridden\n", name);
        return;
    }

    if (name[0] == '\0' || !text_is_safe(name, 0, ":") || !text_is_safe(value, 1, NULL)) {
        fprintf(stderr, "res_set_header: header \"%s\" has an empty name or a control character, dropped\n", name);
        return;
    }

    set_named_value(res, res->headers, &res->header_count, MAX_RESPONSE_HEADERS, name, value, "header");
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

/* RFC 9110 6.4.1 / 8.6: a 1xx, 204 or 304 response never has content, and 1xx/204 must not carry
 * Content-Length (nor Transfer-Encoding). Such a response ends at its head, so every sending path must
 * also drop body bytes - a body sent without framing would be read as the start of the next response. */
static int status_has_no_body(const int status) {
    return status < 200 || status == 204 || status == 304;
}

/* True when no body bytes may follow the head: HEAD (framing headers still describe the GET body) or a
 * bodiless status (no framing headers at all). */
static int body_suppressed(const Response *res) {
    return res->is_head_request || status_has_no_body(res->status);
}

/*
 * Writes "HTTP/1.1 <status> <reason>\r\nDate: ..\r\nContent-Type: ..\r\n" + framing header
 * (Content-Length, or Transfer-Encoding: chunked when body_len == CHUNKED_BODY)
 * + "Connection: ..\r\n" + custom headers + Set-Cookie lines + (Trailer: names)
 * + blank line into buf. Returns the head length, or 0 if it does not fit.
 * A custom Content-Type (res_set_header) replaces `content_type` and is emitted once.
 * Date comes from http_date_for's per-second cache (a 29-byte memcpy per response). A 1xx/204/304
 * gets no framing headers, no Trailer, and no default Content-Type (an explicit one is kept).
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
    bad |= put_str(buf, cap, &off, "\r\nDate: ");
    bad |= put_bytes(buf, cap, &off, http_date_for(time(NULL)), HTTP_DATE_LEN);
    const int no_body = status_has_no_body(res->status);
    if (!no_body || custom_content_type != NULL) {
        bad |= put_str(buf, cap, &off, "\r\nContent-Type: ");
        bad |= put_str(buf, cap, &off, content_type);
    }
    if (no_body) {
        /* no framing: the message ends at the blank line */
    } else if (body_len == CHUNKED_BODY) {
        bad |= put_str(buf, cap, &off, "\r\nTransfer-Encoding: chunked");
    } else {
        bad |= put_str(buf, cap, &off, "\r\nContent-Length: ");
        bad |= put_uint(buf, cap, &off, body_len);
    }
    bad |= put_str(buf, cap, &off, res->conn->keep_alive ? "\r\nConnection: keep-alive\r\n" : "\r\nConnection: close\r\n");

    for (int i = 0; i < res->header_count && !bad; i++) {
        const char *name = res->headers[i].name;
        if (strcasecmp(name, "Content-Type") == 0 ||
            ((body_len == CHUNKED_BODY || no_body) && (strcasecmp(name, "Transfer-Encoding") == 0 ||
                                          strcasecmp(name, "Content-Length") == 0 ||
                                          strcasecmp(name, "Connection") == 0))) {
            continue;
        }
        bad |= put_str(buf, cap, &off, name);
        bad |= put_str(buf, cap, &off, ": ");
        bad |= put_bytes(buf, cap, &off, res->headers[i].value, res->headers[i].value_len);
        bad |= put_str(buf, cap, &off, "\r\n");
    }

    /* One Set-Cookie line per res_set_cookie call: never deduplicated by name. */
    for (int i = 0; i < res->set_cookie_count && !bad; i++) {
        bad |= put_str(buf, cap, &off, "Set-Cookie: ");
        bad |= put_str(buf, cap, &off, res->set_cookies[i]);
        bad |= put_str(buf, cap, &off, "\r\n");
    }

    if (body_len == CHUNKED_BODY && !no_body && res->trailer_count > 0 && !bad) {
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
    stream_release(conn); /* last wins: a producer stream set earlier in this handler is dropped */
    shared_body_detach(conn); /* ... and so is a shared body */

    const size_t head_len = build_response_head(res, content_type, body_len, head, sizeof(head));
    if (head_len == 0) {
        fprintf(stderr, "response head exceeds RESPONSE_HEADER_BUF_SIZE (%d bytes), connection dropped\n",
                RESPONSE_HEADER_BUF_SIZE);
        conn->out_buf = NULL;
        conn->out_cap = 0;
        abort_response(conn); /* headers do not fit: drop rather than send a malformed response */
        return;
    }

    /* Content-Length is always the full body's: a HEAD response reports what GET would
     * (RFC 7231 4.3.2) but sends no body bytes. body == NULL means the body goes out separately
     * (file stream or shared body): head only. A 1xx/204/304 sends neither. */
    const size_t sent_body_len = (body != NULL && !body_suppressed(res)) ? body_len : 0;

    /* A second res_send/res_json in the same request replaces the first response (last wins);
     * the earlier buffer is simply left in the arena to be freed at request end. */

    /* Ownership: managed by the shared per-worker arena, not a per-connection one - reclaimed by
     * arena_reset once this request's dispatch-and-flush cycle ends (handle_readable/reject_request),
     * unless flush_connection has to copy an unsent tail out to a connection-owned buffer first (see
     * Connection.out_buf_owned) because the response couldn't be fully written in one go. */
    conn->out_buf = arena_alloc(conn->arena, head_len + sent_body_len + 1);
    conn->out_buf_owned = 0; /* a fresh arena allocation is never a connection-owned tail-copy */
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

SharedBody *shared_body_new(const size_t len) {
    if (len > SIZE_MAX - sizeof(SharedBody)) {
        return NULL;
    }
    SharedBody *body = malloc(sizeof(SharedBody) + len); /* freed by the shared_body_release that reaches 0 */
    if (body == NULL) {
        return NULL;
    }
    body->refs = 1;
    body->len = len;
    return body;
}

void shared_body_retain(SharedBody *body) {
    body->refs++;
}

void shared_body_release(SharedBody *body) {
    if (body != NULL && --body->refs == 0) {
        free(body);
    }
}

void shared_body_detach(Connection *conn) {
    if (conn == NULL || conn->shared_body == NULL) {
        return;
    }
    shared_body_release(conn->shared_body);
    conn->shared_body = NULL;
    conn->shared_body_sent = 0;
}

void res_send_shared(Response *res, const char *content_type, SharedBody *body) {
    /* body == NULL here builds the head only (the res_send_file convention), with body->len as Content-Length */
    send_with_content_type(res, content_type, NULL, body->len);
    Connection *conn = res->conn;
    if (conn->out_buf == NULL || body_suppressed(res) || body->len == 0) {
        return;
    }
    shared_body_retain(body); /* dropped by shared_body_detach: last byte written, replaced, or connection_close */
    conn->shared_body = body;
    conn->shared_body_sent = 0;
}

void res_redirect(Response *res, int status, const char *location) {
    if (!text_is_safe(location, 1, NULL)) {
        /* A Location with CR/LF is a response-splitting attempt: refuse instead of sending a broken redirect. */
        res_status(res, 500);
        res_send(res, "invalid redirect target");
        return;
    }
    res_set_header(res, "Location", location);
    const char *set = find_header(res, "Location");
    if (set == NULL || strcmp(set, location) != 0) {
        /* Location was not stored (no arena space, header table full): never redirect without it. */
        res_status(res, 500);
        res_send(res, "redirect target could not be set");
        return;
    }
    res_status(res, status != 0 ? status : 302);

    /* body "Redirecting to <location>" in the arena, so a long target is not cut either */
    static const char prefix[] = "Redirecting to ";
    const size_t location_len = strlen(location);
    char *body = arena_alloc(res->conn->arena, sizeof(prefix) - 1 + location_len + 1);
    if (body == NULL) {
        res_send(res, "Redirecting");
        return;
    }
    memcpy(body, prefix, sizeof(prefix) - 1);
    memcpy(body + sizeof(prefix) - 1, location, location_len + 1);
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
        char *grown = arena_alloc(conn->arena, new_cap);
        if (grown == NULL) {
            return -1;
        }
        if (conn->out_buf != NULL && conn->out_len > 0) {
            memcpy(grown, conn->out_buf, conn->out_len);
        }
        conn->out_buf = grown;
        conn->out_buf_owned = 0; /* a fresh arena allocation is never a connection-owned tail-copy */
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
    stream_release(conn); /* last wins: a producer stream set earlier in this handler is dropped */
    shared_body_detach(conn); /* ... and so is a shared body */
    if (res->status == 0) {
        res->status = 200;
    }

    char head[RESPONSE_HEADER_BUF_SIZE];
    const size_t head_len = build_response_head(res, "text/plain", CHUNKED_BODY, head, sizeof(head));
    if (head_len == 0) {
        fprintf(stderr, "response head exceeds RESPONSE_HEADER_BUF_SIZE (%d bytes), connection dropped\n",
                RESPONSE_HEADER_BUF_SIZE);
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
    if (body_suppressed(res)) {
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

    set_named_value(res, res->trailers, &res->trailer_count, MAX_RESPONSE_TRAILERS, name, value, "trailer");
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
    if (body_suppressed(res)) {
        return;
    }
    if (append_to_out_buf(res->conn, "0\r\n", 3) != 0) {
        return;
    }
    for (int i = 0; i < res->trailer_count; i++) {
        const ResponseHeader *tr = &res->trailers[i];
        if (append_to_out_buf(res->conn, tr->name, strlen(tr->name)) != 0 ||
            append_to_out_buf(res->conn, ": ", 2) != 0 ||
            append_to_out_buf(res->conn, tr->value, tr->value_len) != 0 ||
            append_to_out_buf(res->conn, "\r\n", 2) != 0) {
            return;
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

    if (body_suppressed(res) || st.st_size == 0) {
        close(fd);
        res->conn->file_fd = -1;
        res->conn->file_remaining = 0;
    } else {
        res->conn->file_fd = fd;
        res->conn->file_remaining = (size_t)st.st_size;
        res->conn->file_offset = 0;
        res->conn->file_no_sendfile = 0;
    }
    return 0;
}

void stream_release(Connection *conn) {
    if (conn == NULL || conn->stream_fn == NULL) {
        return;
    }
    const StreamCtxFree ctx_free = conn->stream_ctx_free;
    void *const ctx = conn->stream_ctx;
    conn->stream_fn = NULL;
    conn->stream_ctx = NULL;
    conn->stream_ctx_free = NULL;
    conn->stream_paused = 0;
    if (ctx_free != NULL) {
        ctx_free(ctx);
    }
}

int res_stream(Response *res, StreamProducer producer, void *ctx, StreamCtxFree ctx_free) {
    if (res == NULL || res->conn == NULL || producer == NULL || res->headers_sent) {
        return -1;
    }
    if (res->trailer_count > 0) {
        fprintf(stderr, "res_stream: trailers are not supported on a producer stream, dropped\n");
        res->trailer_count = 0;
    }
    if (commit_chunked_headers(res) != 0) {
        return -1;
    }
    res->stream_ended = 1; /* res_write / res_end are no-ops from here on: the producer owns the body */
    if (body_suppressed(res)) {
        if (ctx_free != NULL) {
            ctx_free(ctx);
        }
        return 0;
    }
    Connection *conn = res->conn;
    conn->stream_fn = producer;
    conn->stream_ctx = ctx;
    conn->stream_ctx_free = ctx_free;
    conn->stream_paused = 0;
    return 0;
}

int stream_write(StreamWriter *out, const void *data, size_t len) {
    if (out == NULL || (data == NULL && len > 0)) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    char chunk_hdr[24];
    const int n = snprintf(chunk_hdr, sizeof(chunk_hdr), "%zx\r\n", len);
    if (n <= 0) {
        return -1;
    }
    const size_t framed = (size_t)n + len + 2;
    if (len > out->cap || framed > out->cap - out->len) {
        return -1;
    }
    memcpy(out->buf + out->len, chunk_hdr, (size_t)n);
    memcpy(out->buf + out->len + (size_t)n, data, len);
    memcpy(out->buf + out->len + (size_t)n + len, "\r\n", 2);
    out->len += framed;
    return 0;
}
