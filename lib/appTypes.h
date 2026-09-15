#ifndef APP_TYPES_H
#define APP_TYPES_H

#include <stddef.h>

#define MAX_ROUTES 32
#define MAX_PARAMS 8
#define MAX_CONNECTIONS 16384
#define MAX_EVENTS 64
#define BUF_SIZE 8192
#define DEFAULT_PORT 8080
#define BACKLOG 128

typedef struct {
    char method[8];
    char path[256];
    char query[256];
    char version[16];

    char param_names[MAX_PARAMS][64];
    char param_values[MAX_PARAMS][64];
    int param_count;

    char headers[BUF_SIZE];
    int content_length;
    char body[BUF_SIZE];
} Request;

/* Per-connection state that persists across event-loop turns. */
typedef struct Connection {
    int fd;
    int keep_alive;

    char in_buf[BUF_SIZE];
    size_t in_len;

    char *out_buf;
    size_t out_len;
    size_t out_sent;
} Connection;

typedef struct {
    Connection *conn;
    int status;
} Response;

typedef void (*Handler)(Request *req, Response *res);

typedef struct {
    char method[8];
    char path[256];
    Handler handler;
} Route;

typedef struct {
    Route routes[MAX_ROUTES];
    int route_count;
    int server_fd;
    int kq;
    Connection *connections[MAX_CONNECTIONS];
} App;

#endif /* APP_TYPES_H */
