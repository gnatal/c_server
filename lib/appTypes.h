#ifndef APP_TYPES_H
#define APP_TYPES_H

#include <stddef.h>

#define MAX_ROUTES 32
#define MAX_MIDDLEWARES 16
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

/* req is never mutated by a handler once routing has filled in its params. */
typedef void (*Handler)(const Request *req, Response *res);

typedef struct {
    char method[8];
    char path[256];
    Handler handler;
} Route;

/* Forward-declared: Middleware/ErrorHandler take a MiddlewareChain *, and
 * MiddlewareChain in turn holds a Middleware array, so the chain's tag name
 * has to exist before either function-pointer typedef below. */
typedef struct MiddlewareChain MiddlewareChain;

/* C analogue of Express's (err, req, res, next): the app's single
 * centralized error handler, invoked via chain_error() instead of a thrown
 * exception. */
typedef void (*ErrorHandler)(int status, const char *message, const Request *req, Response *res);

/* App-wide request middleware. Must either continue the pipeline via
 * chain_next(chain), terminate it by writing a response directly
 * (res_send/res_json), or terminate it via chain_error(chain, ...). */
typedef void (*Middleware)(const Request *req, Response *res, MiddlewareChain *chain);

/* One request's walk through the app-wide middleware list, ending at the
 * matched route's Handler (or a 404 if route is NULL). Built fresh per
 * request by dispatch(); middleware advances it via chain_next(). */
struct MiddlewareChain {
    const Middleware *middlewares;
    int count;
    int index;
    Handler final_handler;
    ErrorHandler error_handler;
    const Request *req;
    Response *res;
};

typedef struct {
    Route routes[MAX_ROUTES];
    int route_count;
    Middleware middlewares[MAX_MIDDLEWARES];
    int middleware_count;
    ErrorHandler error_handler;
    int server_fd;
    int kq;
    Connection *connections[MAX_CONNECTIONS];
} App;

#endif /* APP_TYPES_H */
