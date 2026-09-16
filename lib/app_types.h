#ifndef APP_TYPES_H
#define APP_TYPES_H

#include <stddef.h>

#define MAX_ROUTES 32
#define MAX_MIDDLEWARES 16
#define MAX_ROUTE_MIDDLEWARES 8
#define MAX_PARAMS 8
#define MAX_QUERY_PARAMS 16
#define MAX_HEADERS 32
#define MAX_CONNECTIONS 16384
#define MAX_EVENTS 64
#define BUF_SIZE 8192
#define DEFAULT_PORT 8080
#define BACKLOG 128
#define MAX_RESPONSE_HEADERS 16

/*
 * Hard ceiling on a request body's size (Content-Length), independent of and
 * much larger than BUF_SIZE - BUF_SIZE now only bounds the headers (see
 * Connection.in_buf below). A body up to this size is received into a
 * connection's own growable buffer rather than being rejected just for not
 * fitting in BUF_SIZE. 1 MiB, matching a typical small-to-medium JSON API
 * payload limit - an app wanting a tighter cap can still enforce one via
 * middleware (see app/middlewares.c: mw_body_size_guard).
 */
#define MAX_BODY_SIZE (1024 * 1024)

typedef struct {
    char method[8];
    char path[256];
    char query[256];
    char version[16];

    char param_names[MAX_PARAMS][64];
    char param_values[MAX_PARAMS][64];
    int param_count;

    /* Parsed out of `query` (below) by parse_query_string (http_parser.c) at
     * request-parse time, the same fixed-array-plus-count shape as
     * param_names/param_values above - bounded by MAX_QUERY_PARAMS rather
     * than growing, extra pairs past the cap are dropped. A key with no '='
     * gets an empty-string value. Both name and value are URL-decoded
     * (%XX and '+' -> space) - see req_get_query, http_parser.c. */
    char query_names[MAX_QUERY_PARAMS][64];
    char query_values[MAX_QUERY_PARAMS][64];
    int query_count;

    char headers[BUF_SIZE];

    /* Parsed out of `headers` (above) by parse_headers (http_parser.c) at
     * request-parse time, the same fixed-array-plus-count shape as
     * param_names/param_values and query_names/query_values above - bounded
     * by MAX_HEADERS rather than growing, extra headers past the cap are
     * dropped. A malformed line (no ':') is skipped rather than stored.
     * Values are NOT URL-decoded (headers aren't a URL component) - see
     * req_get_header, http_parser.c. */
    char header_names[MAX_HEADERS][64];
    char header_values[MAX_HEADERS][256];
    int header_count;

    int content_length;

    /* Heap-allocated by parse_http_request (http_parser.c), sized to exactly
     * content_length + 1 bytes and owned by whoever called parse_http_request
     * - NULL if content_length is 0 (still points at a valid 1-byte "" in
     * that case, never NULL after a successful parse). Unlike every other
     * Request field, this is not a fixed array: a body can be far larger
     * than any of the engine's other fixed buffers (bounded instead by
     * MAX_BODY_SIZE), so copying it into a BUF_SIZE-capped array the way
     * headers/query/params are would defeat the point. See
     * lib/CLAUDE.md ("Body buffering"). */
    char *body;
} Request;

/* Per-connection state that persists across event-loop turns. */
typedef struct Connection {
    int fd;
    int keep_alive;

    /* Heap-allocated (connection_create), starting at BUF_SIZE and grown via
     * realloc up to header_len + MAX_BODY_SIZE + 1 when a request's declared
     * Content-Length doesn't fit in the current capacity (handle_readable,
     * connection.c) - shrunk back to BUF_SIZE once the connection goes idle
     * again (flush_connection) so one large request doesn't permanently
     * inflate a long-lived keep-alive connection's footprint. in_cap tracks
     * the current allocated size; BUF_SIZE alone no longer bounds it. */
    char *in_buf;
    size_t in_cap;
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
 * route's own middlewares, ending at its Handler (or a 404/405 if route is
 * NULL - see method_not_allowed below). Built fresh per request by
 * dispatch(); middleware advances it via chain_next(). chain->index counts
 * continuously across both arrays - it first exhausts middlewares[0..count),
 * then route_middlewares[0..route_middleware_count), then falls through to
 * final_handler. */
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

    /* Only meaningful when final_handler is NULL (no route matched).
     * dispatch() sets these from match_route_allowed_methods() ahead of
     * time: method_not_allowed is nonzero when req->path matches at least
     * one registered route under a different method, in which case
     * allowed_methods holds that comma-separated method list and
     * chain_next's fallback sends 405 (with an Allow header) instead of
     * 404. allowed_methods points at a buffer owned by dispatch()'s stack
     * frame - valid because chain_next always runs synchronously within
     * that same call, never deferred. */
    int method_not_allowed;
    const char *allowed_methods;
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
