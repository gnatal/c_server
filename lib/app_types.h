#ifndef APP_TYPES_H
#define APP_TYPES_H

#include <stddef.h>
#include <time.h>
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_ROUTES 32
#define MAX_MIDDLEWARES 16
#define MAX_ROUTE_MIDDLEWARES 8
#define MAX_PARAMS 8
#define MAX_QUERY_PARAMS 16
#define MAX_HEADERS 32
#define MAX_EVENTS 64
#define BUF_SIZE 8192
#define DEFAULT_PORT 8080
#define BACKLOG 128
#define MAX_RESPONSE_HEADERS 16
#define MAX_MULTIPART_PARTS 16
#define MAX_COOKIES 16
#define MAX_RESPONSE_COOKIES 16
#define MAX_RESPONSE_TRAILERS 8
#define MAX_FORM_FIELDS 32

/* Bounds App.worker_init_hooks (below) - a small, fixed number of
 * per-process resource-init callbacks, same truncate-with-warning
 * convention as MAX_MIDDLEWARES/MAX_ROUTES. */
#define MAX_WORKER_INIT_HOOKS 4

/* Bounded chunk size for streaming files off disk in flush_connection (connection.c) */
#define STREAM_CHUNK_SIZE (16 * 1024)

/* Bounds one fully-formatted "Set-Cookie" header *value* (name=value plus
 * every attribute - Path, Domain, Max-Age, HttpOnly, Secure, SameSite),
 * built by res_set_cookie (lib/response.h) into Response.set_cookies. */
#define MAX_SET_COOKIE_LEN 512

/* RFC 2046 caps a boundary delimiter at 70 characters; this is that cap
 * plus room for the NUL terminator, used to size boundary buffers
 * (lib/multipart.h) - a Content-Type declaring a longer boundary is
 * rejected by multipart_parse_boundary rather than truncated. */
#define MAX_BOUNDARY_LEN 71

/* Generous upper bound on one "<hex-size>[;ext]" chunk-size line's length in
 * a Transfer-Encoding: chunked request body (lib/http_parser.h:
 * chunked_body_scan/chunked_body_decode) - comfortably fits any legitimate
 * hex chunk size (MAX_BODY_SIZE itself is nowhere near this many hex
 * digits) plus a chunk extension, while still being small enough that a
 * line this long with no CRLF terminator in sight is clearly malformed
 * rather than just "still arriving" (mirrors BUF_SIZE's role for the main
 * header block, at a much smaller scale appropriate to one framing line). */
#define MAX_CHUNK_SIZE_LINE_LEN 64

/*
 * Hard ceiling on a request body's size (Content-Length), independent of and
 * much larger than BUF_SIZE - BUF_SIZE now only bounds the headers (see
 * Connection.in_buf below). A body up to this size is received into a
 * connection's own growable buffer rather than being rejected just for not
 * fitting in BUF_SIZE. 10 MiB, comfortably fitting an ordinary file upload
 * (multipart/form-data, lib/multipart.h) alongside the small-to-medium JSON
 * API payloads this was originally sized for - an app wanting a tighter cap
 * can still enforce one via middleware (see app/middlewares.c:
 * mw_body_size_guard).
 */
#define MAX_BODY_SIZE (10 * 1024 * 1024)

/*
 * Hard ceiling on a file static_serve_file (lib/static.h) will read off disk
 * and hand to res_send_bytes for one request - an existing file larger than
 * this gets a 500 rather than being read into memory whole, since there's no
 * streaming response path yet (see pending.txt item #2, and lib/CLAUDE.md
 * "Static file serving"). 50 MiB, comfortably larger than MAX_BODY_SIZE
 * (which bounds an upload, not a served asset) while still bounded - an app
 * serving larger files needs the streaming response work first.
 */
#define MAX_STATIC_FILE_SIZE (50 * 1024 * 1024)

/*
 * Idle/read timeout: a connection - mid-request (headers or body trickling
 * in below the size limits in MAX_BODY_SIZE/BUF_SIZE above) or between
 * keep-alive requests - that goes this long without the server receiving any
 * bytes from it is closed by the periodic sweep in connection.c
 * (close_idle_connections). This is what actually bounds a Slowloris-shaped
 * client: the buffer-size limits above only reject a request once it's
 * known to be too big, they don't do anything about one that's simply too
 * slow. Deliberately not part of ServerConfig - every other hard limit in
 * this engine (BUF_SIZE, MAX_BODY_SIZE, MAX_ROUTES, ...) is a compile-time
 * constant too, not a runtime knob.
 */
#define IDLE_TIMEOUT_SECONDS 60

/* How often the event loop wakes up (via an EVFILT_TIMER registered in
 * app_listen) to sweep app->connections for timed-out connections - see
 * close_idle_connections, connection.c. Independent of IDLE_TIMEOUT_SECONDS:
 * this only bounds how late a timeout is noticed (up to one interval late),
 * not the timeout duration itself. */
#define IDLE_SWEEP_INTERVAL_MS 1000

/*
 * Maximum time (in seconds) allowed for in-flight requests to complete and
 * flush during graceful shutdown (app_stop, connection.c) before the server
 * force-closes remaining connections and exits. Enforced via a dedicated
 * oneshot EVFILT_TIMER registered upon entering the draining phase.
 */
#define SHUTDOWN_TIMEOUT_SECONDS 5

/*
 * Starting size (in slots) of App.connections (below) - a growable table
 * indexed directly by fd, realloc'd larger by ensure_connection_capacity
 * (connection.c) whenever accept_connections sees an fd that doesn't fit
 * yet. Unlike the fixed-size arrays elsewhere in this engine (MAX_ROUTES,
 * MAX_HEADERS, ...), there is no hard ceiling here - the real bound is the
 * process's own RLIMIT_NOFILE (accept() itself starts failing with EMFILE
 * once that's hit), so a fixed MAX_CONNECTIONS would only ever be either
 * too small (artificially capping the server below what the OS already
 * allows) or wastefully large. This is just the initial allocation, sized
 * for the common case so most servers never need to grow it at all.
 */
#define INITIAL_CONNECTION_TABLE_CAP 1024

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

    /* Parsed out of the "Cookie" header (one of header_values above, found
     * via req_get_header) by parse_cookies (http_parser.c) at
     * request-parse time - same fixed-array-plus-count shape as
     * query_names/query_values. A cookie header with no "Cookie" line at
     * all just leaves cookie_count at 0. Cookie names are case-sensitive
     * per RFC 6265 - req_get_cookie uses strcmp, unlike req_get_header's
     * case-insensitive strcasecmp. Values are NOT URL-decoded, same
     * reasoning as header_values (and because RFC 6265 cookie values are
     * defined over a restricted byte set that doesn't need it). */
    char cookie_names[MAX_COOKIES][64];
    char cookie_values[MAX_COOKIES][256];
    int cookie_count;

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

/*
 * One part of a body parsed by parse_multipart_body (lib/multipart.h) out of
 * a multipart/form-data Request.body. `data` points directly into the
 * buffer that was passed to parse_multipart_body (typically req->body) -
 * it is never copied, so it's only valid as long as that buffer is, and it
 * is NOT NUL-terminated (a file part's payload can contain arbitrary bytes,
 * including embedded NULs) - callers must use data_len, never strlen, to
 * read it. `filename` is left as the empty string ("") for a plain form
 * field and non-empty for a file upload part; `content_type` is likewise
 * empty when the part carried no Content-Type header of its own.
 */
typedef struct {
    char name[128];
    char filename[256];
    char content_type[128];
    const char *data;
    size_t data_len;
} MultipartPart;

/* A parsed multipart/form-data body: a bounded array of parts (see
 * MAX_MULTIPART_PARTS above) - extra parts past the cap are dropped rather
 * than overflowing the fixed array, same convention as
 * MAX_HEADERS/MAX_QUERY_PARAMS elsewhere. */
typedef struct {
    MultipartPart parts[MAX_MULTIPART_PARTS];
    int part_count;
} MultipartForm;

/* A parsed application/x-www-form-urlencoded body - filled by
 * parse_urlencoded_body (lib/urlencoded.h), the same fixed-array-plus-count
 * shape as Request.query_names/query_values (the grammar is identical, just
 * carried in the body instead of the URL) - extra pairs past
 * MAX_FORM_FIELDS are dropped rather than overflowing, same convention as
 * MAX_QUERY_PARAMS/MAX_HEADERS elsewhere. Both name and value are
 * URL-decoded (%XX and '+' -> space). */
typedef struct {
    char field_names[MAX_FORM_FIELDS][64];
    char field_values[MAX_FORM_FIELDS][256];
    int field_count;
} UrlEncodedForm;

typedef enum {
    TLS_STATE_NONE = 0,
    TLS_STATE_HANDSHAKE,
    TLS_STATE_CONNECTED,
    TLS_STATE_CLOSING
} TlsState;

/* Per-connection state that persists across event-loop turns. */
typedef struct Connection {
    int fd;
    int keep_alive;

    /* Wall-clock time of the last successful recv() on this fd (set at
     * connection_create too, so a connection that never sends a single byte
     * is still bounded). Only ever advanced by reads, never by writes - see
     * close_idle_connections (connection.c) for why the write side is
     * deliberately excluded from this timeout. */
    time_t last_activity;

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
    size_t out_cap;

    /* File streaming: when file_fd >= 0, flush_connection streams file_remaining
     * bytes off disk in STREAM_CHUNK_SIZE chunks directly through the event loop
     * without buffering the whole file in RAM. */
    int file_fd;
    size_t file_remaining;

    /* Events actively registered with the event loop backend (EVENT_READ | EVENT_WRITE) */
    int events_watched;

    /* TLS / HTTPS state: opaque handle to SSL * when TLS is active on this connection,
     * NULL for plaintext connections. Tracked via non-blocking handshake states. */
    void *ssl;
    TlsState tls_state;
    int tls_want_read;
    int tls_want_write;
} Connection;

typedef enum {
    EVENT_NONE  = 0,
    EVENT_READ  = 1 << 0,
    EVENT_WRITE = 1 << 1
} EventFlags;

typedef enum {
    LOOP_EVENT_NONE = 0,
    LOOP_EVENT_READ,
    LOOP_EVENT_WRITE,
    LOOP_EVENT_ACCEPT,
    LOOP_EVENT_SIGNAL,
    LOOP_EVENT_TIMER_IDLE,
    LOOP_EVENT_TIMER_SHUTDOWN,
    LOOP_EVENT_ERROR
} LoopEventType;

typedef struct {
    LoopEventType type;
    int fd;
    Connection *conn;
    int signo;
} LoopEvent;


typedef struct {
    char name[64];
    char value[256];
} ResponseHeader;

/* SameSite attribute for res_set_cookie (lib/response.h) - COOKIE_SAMESITE_UNSET
 * (the zero value, so a zero-initialized CookieOptions defaults to it) omits
 * the SameSite attribute entirely rather than emitting an empty one. */
typedef enum {
    COOKIE_SAMESITE_UNSET = 0,
    COOKIE_SAMESITE_STRICT,
    COOKIE_SAMESITE_LAX,
    COOKIE_SAMESITE_NONE
} CookieSameSite;

/*
 * Options for res_set_cookie (lib/response.h), mirroring a subset of
 * Express's res.cookie(name, value, options). A zero-initialized
 * CookieOptions (e.g. `(CookieOptions){0}`) means: session cookie (no
 * Max-Age - max_age must be set to a non-negative value to emit one; 0 is
 * a valid explicit value, used by res_clear_cookie to expire a cookie
 * immediately, so "unset" can't just be 0), no Domain, not HttpOnly/Secure,
 * no SameSite attribute. A NULL path defaults to "/" (res_set_cookie's own
 * default - not the browser's implicit "current request path" default,
 * which is rarely what a handler wants).
 */
typedef struct {
    int max_age;
    const char *path;
    const char *domain;
    int http_only;
    int secure;
    CookieSameSite same_site;
} CookieOptions;

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

    /* Set via res_set_cookie() (response.c): each entry is one fully
     * pre-formatted Set-Cookie header value (e.g. "name=value; Path=/;
     * HttpOnly"), sent as its own "Set-Cookie: ..." line by
     * send_with_content_type. Unlike headers[] above, these can't share
     * that same-name-overwrites array - a response can legitimately carry
     * more than one Set-Cookie line at once (one per cookie), and
     * res_set_cookie never deduplicates by name. Bounded by
     * MAX_RESPONSE_COOKIES; extra calls past the cap are dropped (stderr
     * warning), same convention as headers[]/MAX_RESPONSE_HEADERS. */
    char set_cookies[MAX_RESPONSE_COOKIES][MAX_SET_COOKIE_LEN];
    int set_cookie_count;

    /* Set by handle_readable (connection.c) from req->method before dispatch
     * runs, whenever a request was successfully parsed. HTTP requires a HEAD
     * response to carry the same headers (including Content-Length) a GET
     * would have produced, but never a body, regardless of status (RFC 7231
     * 4.3.2) - send_with_content_type (response.c) still computes
     * Content-Length from the handler's body string, it just skips copying
     * those bytes into conn->out_buf. Left at 0 (default) on paths that
     * never reach a full parse (e.g. the 431/408 rejections in
     * connection.c), which is safe - those responses have no real body to
     * suppress either way. */
    int is_head_request;

    /* Chunked response trailers (res_set_trailer, response.c), emitted after
     * the terminating 0\r\n chunk per RFC 7230 §4.1.2. */
    ResponseHeader trailers[MAX_RESPONSE_TRAILERS];
    int trailer_count;

    /* Chunked streaming response state (res_write / res_end): */
    int is_chunked;
    int headers_sent;
    int stream_ended;
} Response;

/* req is never mutated by a handler once routing has filled in its params. */
typedef void (*Handler)(const Request *req, Response *res);

/*
 * Registered via app_on_worker_start (lib/connection.h) and run once per
 * worker process, from app_listen_worker (lib/connection.c) - see
 * lib/CLAUDE.md ("Worker lifecycle hooks") for why this exists: a resource
 * opened in main() before app_listen() gets duplicated into every forked
 * cluster worker (lib/cluster.c) along with the rest of that process's
 * memory, which is unsafe for some resources (e.g. a SQLite connection).
 * This hook lets an app (re)initialize such a resource in whichever process
 * actually ends up serving requests, standalone or forked, instead.
 */
typedef void (*WorkerInitHook)(void);

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

    /* NULL for a static-file route (see static_root below) - dispatch()
     * (middleware.c) never calls a NULL handler, it branches to
     * static_serve_file (lib/static.h) instead once it sees static_root is
     * non-empty. Every other route always has a real handler here. */
    Handler handler;

    /* Per-route middleware, registered via app_get_mw/app_post_mw. Runs in
     * dispatch() after the app-wide middlewares and before this handler.
     * Fixed-size + count (like route_count/middleware_count/param_count
     * elsewhere) rather than a NULL-terminated variadic, so a forgotten
     * sentinel can't silently under-run the array. */
    Middleware middlewares[MAX_ROUTE_MIDDLEWARES];
    int middleware_count;

    /* Empty ("") for an ordinary route - set only by app_serve_static
     * (lib/router.h) to the canonicalized (realpath'd) absolute directory a
     * static-file mount serves from. A route with a non-empty static_root
     * has `path` always ending in a trailing wildcard segment
     * (app_serve_static's own doing) and a NULL handler -
     * dispatch()/chain_next() (lib/middleware.c) check this field, not
     * `handler`, to tell a static mount apart from a route with no handler
     * by mistake. See lib/CLAUDE.md ("Static file serving"). */
    char static_root[PATH_MAX];
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

    /* The matched Route itself (NULL if none matched) - final_handler above
     * is just route->handler, which is NULL for a static-file route
     * (Route.static_root, app_types.h). chain_next's final fallback
     * (middleware.c) checks this field ahead of final_handler so it can
     * dispatch to static_serve_file (lib/static.h) instead of trying to call
     * a NULL Handler. */
    const Route *route;
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
    int workers; /* 1 = single process (default); > 1 = cluster mode; 0 = auto-detect CPU cores */
    int tls_enabled;
    char tls_cert_file[PATH_MAX];
    char tls_key_file[PATH_MAX];
} ServerConfig;

typedef struct {
    ServerConfig config;
    Route routes[MAX_ROUTES];
    int route_count;
    MiddlewareEntry middlewares[MAX_MIDDLEWARES];
    int middleware_count;
    ErrorHandler error_handler;
    int server_fd;
    union {
        int kq;
        int epoll_fd;
        int loop_fd;
    };
    int timer_idle_fd;
    int timer_shutdown_fd;
    int signal_fd;

    /* TLS / HTTPS SSL_CTX handle: initialized if config.tls_enabled, NULL otherwise. */
    void *ssl_ctx;

    /* Heap-allocated (app_init), indexed directly by fd - a Connection* for
     * an open connection, NULL otherwise. Starts at
     * INITIAL_CONNECTION_TABLE_CAP slots and is grown (realloc, doubling) by
     * ensure_connection_capacity (connection.c) whenever accept_connections
     * sees an fd that doesn't fit yet - connections_cap tracks the current
     * allocated slot count. Freed by app_destroy (connection.h), which
     * matches this malloc per this engine's memory-lifecycle convention. */
    Connection **connections;
    int connections_cap;

    /* Registered via app_on_worker_start (lib/connection.h), run in
     * registration order from the top of app_listen_worker
     * (lib/connection.c) - once per worker process, always after any
     * cluster fork (lib/cluster.c) has already happened. See
     * lib/CLAUDE.md ("Worker lifecycle hooks"). */
    WorkerInitHook worker_init_hooks[MAX_WORKER_INIT_HOOKS];
    int worker_init_hook_count;

    /* Graceful shutdown state: set to 1 by app_stop (connection.c) when a
     * SIGINT/SIGTERM arrives or shutdown is initiated programmatically.
     * Tells handle_readable/flush_connection to close idle connections,
     * set Connection: close on responses, and terminate the event loop
     * once open connections reach 0 or timeout expires. */
    int is_shutting_down;
} App;

#endif /* APP_TYPES_H */
