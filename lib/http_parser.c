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
    if (value < 0 || value > BUF_SIZE - 1) {
        return -1;
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
    url_decode(req->path, req->path, sizeof(req->path), 0);
    parse_query_string(req->query, req);

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
    if (req->content_length < 0) {
        return -1;
    }

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

const char *status_text(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        default:  return "Unknown";
    }
}
