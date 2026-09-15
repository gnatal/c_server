#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "httpParser.h"

int extract_content_length(const char *header_block) {
    const char *cl_header = strcasestr(header_block, "Content-Length:");
    if (cl_header == NULL) {
        return 0;
    }
    return atoi(cl_header + strlen("Content-Length:"));
}

int request_is_complete(const char *buf, size_t len) {
    const char *header_end = strstr(buf, "\r\n\r\n");
    if (header_end == NULL) {
        return 0;
    }

    size_t header_len = (size_t)((header_end + 4) - buf);
    size_t body_have = len - header_len;
    const int content_length = extract_content_length(buf);

    return (long)body_have >= content_length;
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

int parse_http_request(const char *raw, Request *req) {
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
    strncpy(req->path, full_path, sizeof(req->path) - 1);

    char *header_start = strstr(raw, "\r\n");
    char *header_end = strstr(raw, "\r\n\r\n");
    if (header_start == NULL || header_end == NULL) {
        return -1;
    }
    header_start += 2;

    size_t header_len = (size_t)(header_end - header_start);
    if (header_len >= sizeof(req->headers)) {
        header_len = sizeof(req->headers) - 1;
    }
    memcpy(req->headers, header_start, header_len);
    req->headers[header_len] = '\0';

    req->content_length = extract_content_length(req->headers);

    char *body_start = header_end + 4;
    size_t available = strlen(body_start);
    size_t body_len = (size_t)req->content_length;
    if (body_len > available) {
        body_len = available;
    }
    if (body_len >= sizeof(req->body)) {
        body_len = sizeof(req->body) - 1;
    }
    memcpy(req->body, body_start, body_len);
    req->body[body_len] = '\0';

    return 0;
}

const char *status_text(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 500: return "Internal Server Error";
        default:  return "Unknown";
    }
}
