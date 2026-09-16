#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static void send_with_content_type(Response *res, const char *content_type, const char *body) {
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
        res->status, status_text(res->status), content_type, strlen(body),
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
    const size_t body_len = strlen(body);
    /* Content-Length above is always computed from the full body - a HEAD
     * response must report the same length a GET would have (RFC 7231
     * 4.3.2), it just never actually sends those bytes. */
    const size_t sent_body_len = res->is_head_request ? 0 : body_len;

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
}

void res_send(Response *res, const char *body) {
    send_with_content_type(res, "text/plain", body);
}

void res_json(Response *res, const char *body) {
    send_with_content_type(res, "application/json", body);
}
