#ifndef APP_TYPES_H
#define APP_TYPES_H

/*
 * Every shared type and compile-time limit of the engine. Limits are constants, not runtime knobs:
 * anything past a limit is truncated or dropped (with a stderr warning for registrations), never
 * overflowed. Field arrays are read only up to their *_count; never index past it.
 */

#include <stddef.h>
#include <time.h>
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* ---- limits ---- */
#define MAX_ROUTES 32                 /* per App and per Router */
#define MAX_MIDDLEWARES 16            /* app-wide and per Router */
#define MAX_ROUTE_MIDDLEWARES 8       /* per route */
#define MAX_PARAMS 8                  /* path params per request (name/value 63 chars) */
#define MAX_QUERY_PARAMS 16           /* name/value 63 chars, percent- and '+'-decoded */
#define MAX_HEADERS 32                /* request headers kept (name 63, value 255 chars) */
#define MAX_COOKIES 16                /* request cookies kept (name 63, value 255 chars) */
#define MAX_FORM_FIELDS 32            /* urlencoded body fields (name 63, value 255 chars) */
#define MAX_MULTIPART_PARTS 16
#define MAX_RESPONSE_HEADERS 16       /* name 63, value 255 chars */
#define MAX_RESPONSE_COOKIES 16
#define MAX_SET_COOKIE_LEN 512        /* one formatted Set-Cookie value; longer is dropped, not truncated */
#define MAX_RESPONSE_TRAILERS 8
#define MAX_WORKER_INIT_HOOKS 4
#define MAX_BOUNDARY_LEN 71           /* RFC 2046: 70 chars + NUL */
#define MAX_CHUNK_SIZE_LINE_LEN 64    /* a chunk-size line longer than this is malformed */
#define MAX_EVENTS 64                 /* events fetched per event-loop poll */
#define BACKLOG 128
#define DEFAULT_PORT 8080

#define BUF_SIZE 8192                 /* connection input buffer start size; also the request-header limit (431 beyond) */
#define MAX_BODY_SIZE (10 * 1024 * 1024)        /* request body (Content-Length or decoded chunked) and streamed response buffer; 413 beyond */
#define MAX_STATIC_FILE_SIZE (50 * 1024 * 1024) /* static_serve_file refuses larger files with 500 */
#define STREAM_CHUNK_SIZE (16 * 1024)           /* res_send_file reads this much per write; 4 chunks per event-loop turn */
#define IDLE_TIMEOUT_SECONDS 60       /* no bytes received for this long: close (408 if mid-request) */
#define IDLE_SWEEP_INTERVAL_MS 1000   /* how often idle connections are checked */
#define SHUTDOWN_TIMEOUT_SECONDS 5    /* graceful-drain deadline before force close */
#define INITIAL_CONNECTION_TABLE_CAP 1024 /* App.connections slots at start; doubles on demand (bounded by RLIMIT_NOFILE) */

/* ---- request ---- */

/*
 * One parsed request, filled by parse_http_request (http_parser.h) and read-only for handlers.
 * Read it through the accessors (req_get_param / req_get_query / req_get_header / req_get_cookie),
 * not by indexing the arrays. Only fields behind a *_count (and the scalars) are initialized.
 * Lifetime: valid until the handler returns; do not keep pointers into it.
 */
typedef struct {
    char method[8];          /* "GET", ... up to 7 chars */
    char path[256];          /* percent-decoded, no query string */
    char query[256];         /* raw text after '?', truncated to 255 */
    char version[16];        /* "HTTP/1.1" */

    char param_names[MAX_PARAMS][64];       /* ':name' captures, filled by match_route */
    char param_values[MAX_PARAMS][64];
    int param_count;

    char query_names[MAX_QUERY_PARAMS][64];
    char query_values[MAX_QUERY_PARAMS][64];
    int query_count;

    char header_names[MAX_HEADERS][64];     /* value not decoded; name lookup is case-insensitive */
    char header_values[MAX_HEADERS][256];
    int header_count;

    char cookie_names[MAX_COOKIES][64];     /* not decoded; name lookup is case-sensitive */
    char cookie_values[MAX_COOKIES][256];
    int cookie_count;

    int content_length;      /* body size in bytes (decoded size for chunked); -2 after a failed parse = too large */

    /* malloc'd by parse_http_request (never NULL after success, NUL-terminated, may contain NUL bytes:
     * use content_length, not strlen). Freed by the engine after dispatch; handlers must not free it. */
    char *body;
} Request;

/* One multipart/form-data part. `data` points INTO the parsed body buffer, is not NUL-terminated and
 * may contain NULs: use data_len. `filename` is "" for a plain field; content_type "" when absent. */
typedef struct {
    char name[128];
    char filename[256];
    char content_type[128];
    const char *data;
    size_t data_len;
} MultipartPart;

typedef struct {
    MultipartPart parts[MAX_MULTIPART_PARTS];
    int part_count;
} MultipartForm;

/* Parsed application/x-www-form-urlencoded body: names and values are copied and decoded. */
typedef struct {
    char field_names[MAX_FORM_FIELDS][64];
    char field_values[MAX_FORM_FIELDS][256];
    int field_count;
} UrlEncodedForm;

/* ---- connection and event loop ---- */

typedef enum {
    TLS_STATE_NONE = 0,
    TLS_STATE_HANDSHAKE,
    TLS_STATE_CONNECTED,
    TLS_STATE_CLOSING
} TlsState;

/* Per-connection state, one per accepted fd, owned by App.connections[fd]. Freed only by connection_close. */
typedef struct Connection {
    int fd;
    int keep_alive;

    time_t last_activity;   /* last successful recv (or accept); writes do not refresh it */

    /* Input: malloc'd at BUF_SIZE, realloc'd up to header_len + MAX_BODY_SIZE + 1 to fit a declared
     * body, shrunk back to BUF_SIZE when the connection goes idle. in_buf[in_len] is always '\0'. */
    char *in_buf;
    size_t in_cap;
    size_t in_len;

    /* Output: one malloc'd response built by res_send / res_json / res_write, freed by flush_connection (keep-alive
     * done) or connection_close. out_buf != NULL means a response is pending. For responses built by
     * the response layer, out_buf[out_len] == '\0' (not sent). */
    char *out_buf;
    size_t out_len;
    size_t out_sent;
    size_t out_cap;

    /* File streaming (res_send_file): >= 0 while flush_connection streams file_remaining bytes from disk. */
    int file_fd;
    size_t file_remaining;

    int events_watched;     /* EVENT_READ | EVENT_WRITE currently registered with the event loop */

    void *ssl;              /* SSL* when TLS is active, else NULL */
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

/* ---- response ---- */

typedef struct {
    char name[64];
    char value[256];
} ResponseHeader;

/* SameSite for res_set_cookie; UNSET (zero) omits the attribute. */
typedef enum {
    COOKIE_SAMESITE_UNSET = 0,
    COOKIE_SAMESITE_STRICT,
    COOKIE_SAMESITE_LAX,
    COOKIE_SAMESITE_NONE
} CookieSameSite;

/* res_set_cookie options. The zero value ({0}) is a plain session cookie: Path=/, no Max-Age/Domain/
 * HttpOnly/Secure/SameSite. Or pass options = NULL for the same thing. */
typedef struct {
    int max_age;             /* > 0: lifetime in seconds; 0: session cookie (no Max-Age); < 0: expire now */
    const char *path;        /* NULL -> "/" */
    const char *domain;      /* NULL omits */
    int http_only;
    int secure;
    CookieSameSite same_site;
} CookieOptions;

/*
 * The response being built for one request. Create it with res_init (response.h), fill it through
 * res_* functions only. Sending builds bytes into conn->out_buf; nothing here touches the socket.
 * Arrays are read only up to their *_count.
 */
typedef struct {
    Connection *conn;
    int status;

    ResponseHeader headers[MAX_RESPONSE_HEADERS]; /* res_set_header; Content-Length/Connection are managed by the engine */
    int header_count;

    char set_cookies[MAX_RESPONSE_COOKIES][MAX_SET_COOKIE_LEN]; /* one formatted "Set-Cookie" value each */
    int set_cookie_count;

    int is_head_request;     /* set by the engine before dispatch: Content-Length is sent, the body is not */

    ResponseHeader trailers[MAX_RESPONSE_TRAILERS]; /* res_set_trailer: sent after the last chunk */
    int trailer_count;

    int is_chunked;          /* streaming state: res_write / res_end */
    int headers_sent;
    int stream_ended;
} Response;

/* ---- handlers, middleware, routing ---- */

/* Route handler. Terminal: produces exactly one response via res_*. Cannot call chain_next/chain_error. */
typedef void (*Handler)(const Request *req, Response *res);

/* Runs once per serving process after fork (app_on_worker_start): open per-process resources here. */
typedef void (*WorkerInitHook)(void);

typedef struct MiddlewareChain MiddlewareChain;

/* Middleware: call chain_next(chain), or chain_error(chain, ...), or respond itself. See middleware.h. */
typedef void (*Middleware)(const Request *req, Response *res, MiddlewareChain *chain);

/* App-wide middleware slot. prefix "" or "/" matches every path; otherwise req->path must start with
 * the prefix at a segment boundary. */
typedef struct {
    Middleware fn;
    char prefix[128];
} MiddlewareEntry;

typedef struct {
    char method[8];
    char path[256];               /* pattern, e.g. "/users/:id" */
    Handler handler;              /* NULL for a static-file mount (see static_root) */
    Middleware middlewares[MAX_ROUTE_MIDDLEWARES];
    int middleware_count;
    char static_root[PATH_MAX];   /* "" for an ordinary route; canonical directory for app_serve_static mounts */
} Route;

/* A standalone route table, copied into an App by app_mount. It has no life at dispatch time. */
typedef struct {
    Route routes[MAX_ROUTES];
    int route_count;
    Middleware middlewares[MAX_MIDDLEWARES];
    int middleware_count;
} Router;

/* The single app-wide error sink for chain_error (app_use_error). */
typedef void (*ErrorHandler)(int status, const char *message, const Request *req, Response *res);

/* One request's walk through the pipeline, built by dispatch() on its stack and advanced by chain_next.
 * index counts across app-wide entries, then route middleware, then the handler / default answer. */
struct MiddlewareChain {
    const MiddlewareEntry *middlewares;
    int count;
    const Middleware *route_middlewares;
    int route_middleware_count;
    int index;
    Handler final_handler;
    const Route *route;            /* matched route or NULL; checked first for a static mount */
    ErrorHandler error_handler;
    const Request *req;
    Response *res;
    int method_not_allowed;        /* no route, but the path exists under another method (405 / OPTIONS answer) */
    const char *allowed_methods;   /* "GET, POST" for the Allow header; owned by dispatch()'s frame */
};

/* ---- app ---- */

typedef struct {
    int port;
    int workers;        /* 1 = single process (default); > 1 = cluster; 0 = one worker per CPU core */
    int tls_enabled;
    char tls_cert_file[PATH_MAX];
    char tls_key_file[PATH_MAX];
} ServerConfig;

/* The whole server. ~48 KB: declare it static or on main's stack. Create with app_init, end with app_destroy. */
typedef struct {
    ServerConfig config;
    Route routes[MAX_ROUTES];
    int route_count;
    MiddlewareEntry middlewares[MAX_MIDDLEWARES];
    int middleware_count;
    ErrorHandler error_handler;
    int server_fd;
    union {
        int kq;          /* macOS/BSD */
        int epoll_fd;    /* Linux */
        int loop_fd;     /* either, platform-neutral name */
    };
    int timer_idle_fd;
    int timer_shutdown_fd;
    int signal_fd;

    void *ssl_ctx;       /* SSL_CTX* once TLS is initialized, else NULL */

    /* Indexed directly by fd: the Connection* for an open fd, else NULL. Heap-allocated by app_init,
     * grown by doubling in accept_connections, freed by app_destroy. */
    Connection **connections;
    int connections_cap;

    WorkerInitHook worker_init_hooks[MAX_WORKER_INIT_HOOKS];
    int worker_init_hook_count;

    int is_shutting_down;  /* set by app_stop: no new connections, responses carry Connection: close */
} App;

#endif /* APP_TYPES_H */
