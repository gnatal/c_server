#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "response.h"
#include "httpParser.h"

void res_status(Response *res, int status) {
    res->status = status;
}

static void send_with_content_type(Response *res, const char *content_type, const char *body) {
    char header[512];
    Connection *conn = res->conn;
    const int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: %s\r\n"
        "\r\n",
        res->status, status_text(res->status), content_type, strlen(body),
        conn->keep_alive ? "keep-alive" : "close");

    const size_t body_len = strlen(body);

    conn->out_buf = malloc((size_t)header_len + body_len);
    memcpy(conn->out_buf, header, (size_t)header_len);
    memcpy(conn->out_buf + header_len, body, body_len);
    conn->out_len = (size_t)header_len + body_len;
    conn->out_sent = 0;
}

void res_send(Response *res, const char *body) {
    send_with_content_type(res, "text/plain", body);
}

void res_json(Response *res, const char *body) {
    send_with_content_type(res, "application/json", body);
}
