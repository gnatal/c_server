#ifndef APP_TYPES_H
#define APP_TYPES_H

#include <stddef.h>

#define MAX_ROUTES 32
#define MAX_MIDDLEWARES 16
#define MAX_ROUTE_MIDDLEWARES 8
#define MAX_PARAMS 8
#define MAX_CONNECTIONS 16384
#define MAX_EVENTS 64
#define BUF_SIZE 8192
#define DEFAULT_PORT 8080
#define BACKLOG 128
#define MAX_RESPONSE_HEADERS 16

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
    char name[64];
    char value[256];
} ResponseHeader;

typedef struct {
    Connection *conn;
    int status;

    /* Extra headers set via res_set_header(), sent in addition to the
     * Content-Type/Content-Length/Connection headers that res_send/res_json
     * always emit. "Content-Length" and "Connection" are reserved - the
     * response layer computes those itself, so res_set_header() rejects
     * attempts to override them. */
    ResponseHeader headers[MAX_RESPONSE_HEADERS];
    int header_count;
} Response;

/* req is never mutated by a handler once routing has filled in its params. */
typedef void (*Handler)(const Request *req, Response *res);

/* Forward-declared: Middleware/ErrorHandler take a MiddlewareChain *, and
 * MiddlewareChain in turn holds a Middleware array, so the chain's tag name
 * has to exist before either function-pointer typedef below. Declared here
 * (ahead of Route) so Route can carry its own fixed Middleware array. */
typedef struct MiddlewareChain MiddlewareChain;

/* Request middleware (app-wide via app_use, or per-route via app_get_mw/
 * app_post_mw). Must either continue the pipeline via chain_next(chain),
 * terminate it by writing a response directly (res_send/res_json), or
 * terminate it via chain_error(chain, ...). */
typedef void (*Middleware)(const Request *req, Response *res, MiddlewareChain *chain);

/* An app-wide middleware slot (app->middlewares) pairs the function with the
 * path prefix it's mounted at. prefix == "" or "/" means unscoped - it runs
 * on every request, same as before prefix-scoping existed. A non-empty
 * prefix (e.g. "/api") only runs when req->path starts with it at a segment
 * boundary (see middleware_prefix_matches in middleware.c). */
typedef struct {
    Middleware fn;
    char prefix[128];
} MiddlewareEntry;

typedef struct {
    char method[8];
    char path[256];
    Handler handler;

    /* Per-route middleware, registered via app_get_mw/app_post_mw. Runs in
     * dispatch() after the app-wide middlewares and before this handler.
     * Fixed-size + count (like route_count/middleware_count/param_count
     * elsewhere) rather than a NULL-terminated variadic, so a forgotten
     * sentinel can't silently under-run the array. */
    Middleware middlewares[MAX_ROUTE_MIDDLEWARES];
    int middleware_count;
} Route;

/* A standalone, unmounted route table - the C analogue of Express's
 * Router(). Built up via router_get/router_post/router_use exactly like an
 * App is via app_get/app_post/app_use, but registers nothing against any
 * App until app_mount(app, prefix, router) flattens it in: each Route's path
 * is copied into app->routes with prefix prepended, and each router-level
 * Middleware is copied into app->middlewares scoped to that same prefix
 * (see app_mount, router.c). A Router has no existence at request-dispatch
 * time - only the App it was mounted into does. */
typedef struct {
    Route routes[MAX_ROUTES];
    int route_count;
    Middleware middlewares[MAX_MIDDLEWARES];
    int middleware_count;
} Router;

/* C analogue of Express's (err, req, res, next): the app's single
 * centralized error handler, invoked via chain_error() instead of a thrown
 * exception. */
typedef void (*ErrorHandler)(int status, const char *message, const Request *req, Response *res);

/* One request's walk through the app-wide middlewares, then the matched
 * route's own middlewares, ending at its Handler (or a 404 if route is
 * NULL). Built fresh per request by dispatch(); middleware advances it via
 * chain_next(). chain->index counts continuously across both arrays - it
 * first exhausts middlewares[0..count), then route_middlewares[0..
 * route_middleware_count), then falls through to final_handler. */
struct MiddlewareChain {
    const MiddlewareEntry *middlewares;
    int count;
    const Middleware *route_middlewares;
    int route_middleware_count;
    int index;
    Handler final_handler;
    ErrorHandler error_handler;
    const Request *req;
    Response *res;
};

typedef struct {
    int port;
} ServerConfig;

typedef struct {
    ServerConfig config;
    Route routes[MAX_ROUTES];
    int route_count;
    MiddlewareEntry middlewares[MAX_MIDDLEWARES];
    int middleware_count;
    ErrorHandler error_handler;
    int server_fd;
    int kq;
    Connection *connections[MAX_CONNECTIONS];
} App;

#endif /* APP_TYPES_H */
