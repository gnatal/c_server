#ifndef APP_TYPES_H
#define APP_TYPES_H

/*
 * Every shared type and compile-time limit of the engine. Limits are constants, not runtime knobs:
 * anything past a limit is truncated or dropped (with a stderr warning for registrations), never
 * overflowed. Field arrays are read only up to their *_count; never index past it.
 */

#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <limits.h>
#include "arena.h"
#include "vendor/picohttpparser/picohttpparser.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* on macOS/BSD, per-worker SO_REUSEPORT listen sockets do not balance accepted connections across
 * workers the way Linux's 4-tuple hashing does (improvements.md - MEASURED over 90% of load
 * landing on one of four workers). Gated to non-Linux only: Linux keeps its existing, unmeasured-but-
 * not-reported-broken per-worker accept path untouched, matching the same "narrow, platform-scoped
 * fix" precedent as the io_uring/kqueue split below. Under this macro, only the master process binds
 * and accept()s the listening socket (cluster.c: cluster_listen); each accepted fd is handed to a
 * worker over a per-worker AF_UNIX SOCK_STREAM socketpair via SCM_RIGHTS, round-robin across active
 * workers (see App.accept_via_fd_passing below and lib/CLAUDE.md, "Workers and fork"). */
#if !defined(__linux__)
#define CEXPRESS_SINGLE_ACCEPTOR 1
#endif

/* ---- limits ---- */
#define MAX_ROUTER_ROUTES 64          /* per Router (sub-router staging limit) */
#define MAX_MIDDLEWARES 16            /* app-wide and per Router */
#define MAX_BODY_LIMITS 16            /* app_use_body_limit prefixes; excess dropped with a stderr warning */
#define MAX_ROUTE_MIDDLEWARES 8       /* per route */
#define MAX_PARAMS 8                  /* path params per request (name/value 63 chars) */
#define MAX_QUERY_PARAMS 16           /* name/value 63 chars, percent- and '+'-decoded */
#define MAX_HEADERS 32                /* request header VIEWS kept (not copies - name/value point into
                                        * the connection's in_buf, so there is no per-header size cap left to
                                        * enforce; a header is bounded only by the whole header block fitting
                                        * in BUF_SIZE, same as before) */
#define MAX_FRAMING_HEADERS 100       /* ParsedHead's phr_parse_request capacity: kept above MAX_HEADERS
                                        * so a request with more headers than the engine stores is diagnosed
                                        * by parse_http_request_from_head's explicit ">MAX_HEADERS" check as
                                        * malformed (400), not mis-reported as "incomplete" by
                                        * request_head_is_complete's header_len == 0 check (which a smaller
                                        * capacity would trip via phr_parse_request's own internal error -
                                        * see the malformed-request note in lib/CLAUDE.md, which this must not widen) */
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
#define MAX_PIPELINED_PER_EVENT 16    /* requests served back to back from one connection's buffer
                                        * before yielding to the event loop (fairness across connections) */
#define BACKLOG 128                   /* listen() backlog floor; create_server_socket takes max(BACKLOG, SOMAXCONN) */
#define DEFAULT_PORT 8080
#define DEFAULT_MAX_CONNECTIONS 10000 /* ServerConfig.max_connections default, applied by app_init (see below) */

#define ARENA_SIZE (64 * 1024)        /* App.arena's fixed buffer: one shared per-worker bump allocator, not one
                                        * per connection; reset once per request, falls back to malloc beyond this */
#define BUF_SIZE 8192                 /* App.read_buf and an owned in_buf's start size; also the request-header limit (431 beyond) */
#define MAX_BODY_SIZE (10 * 1024 * 1024)        /* request body (Content-Length or decoded chunked) and streamed response buffer; 413 beyond */
#define MAX_STATIC_FILE_SIZE (50 * 1024 * 1024) /* static_serve_file refuses larger files with 500 */
#define STATIC_CACHE_MAX_ENTRIES 256            /* distinct cached files per process (static.c); LRU-by-staleness eviction beyond it */
#define STATIC_CACHE_MAX_ENTRY_BYTES (256 * 1024)      /* a file bigger than this is served but never cached */
#define STATIC_CACHE_MAX_TOTAL_BYTES (64 * 1024 * 1024) /* combined cap across all cached entries; evicts the stalest first */
#define STATIC_CACHE_REVALIDATE_SECONDS 1       /* a cache hit within this long of its last stat skips the filesystem entirely */
#define STREAM_CHUNK_SIZE (16 * 1024)           /* res_send_file reads this much per write, and a res_stream producer
                                                 * fills at most this much per call; 4 chunks per event-loop turn */
#define STREAM_WRITE_MAX (STREAM_CHUNK_SIZE - 16) /* largest single stream_write that always fits an empty
                                                   * turn buffer (chunk framing + the last-chunk reserve) */
#define IDLE_TIMEOUT_SECONDS 60       /* no bytes received for this long: close (408 if mid-request) */
#define REQUEST_HEADER_TIMEOUT_SECONDS 10  /* deadline from the first byte of a request to a complete header
                                             * block, regardless of how often the client sends a byte: closes
                                             * slow-drip connections IDLE_TIMEOUT_SECONDS alone cannot
                                             * (last_activity resets on every byte, however sparse) */
#define REQUEST_BODY_TIMEOUT_SECONDS 30    /* deadline from the same first byte to a fully framed body once headers
                                             * are complete; same slow-drip rationale, sized for MAX_BODY_SIZE */
#define WRITE_TIMEOUT_SECONDS 30      /* a pending response that hasn't accepted a single byte onto the socket for
                                        * this long is closed: a slow reader is fine, one making zero progress at
                                        * all is a client that stopped reading and would otherwise pin the
                                        * connection (fd, arena, out_buf) forever */
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

    /* raw VIEWS into the connection's in_buf, not copies - name/value are not NUL-terminated and
     * are meaningless once in_buf is touched again (never happens before the handler returns; see
     * lib/CLAUDE.md's request lifecycle). A request whose headers nobody reads costs nothing beyond
     * this array (32 * sizeof(struct phr_header) = 1,024 bytes) instead of the old 32 * 1,088-byte
     * fixed-size copy. req_get_header is the only supported way to read one: it materializes a
     * NUL-terminated copy into `arena` below on every call (not cached - handlers read a given header
     * at most a handful of times, so re-copying costs less than the bookkeeping a cache would need). */
    struct phr_header headers[MAX_HEADERS];
    int header_count;

    /* Cookie splitting stays eager-into-fixed-arrays (unlike headers above): MAX_COOKIES * (64 + 256)
     * is a modest 5 KB regardless, and RFC 6265 cookie-pair syntax has no length limit of its own to
     * relax the way headers needed. What *is* lazy is running the split at all: see
     * cookies_parsed below - most requests carrying a Cookie header are never asked for one by name. */
    char cookie_names[MAX_COOKIES][64];     /* not decoded; name lookup is case-sensitive */
    char cookie_values[MAX_COOKIES][256];
    int cookie_count;
    int cookies_parsed;       /* req_get_cookie runs parse_cookies at most once, on its first call,
                                * instead of parse_http_request_from_head running it for every request
                                * whether or not a handler ever reads a cookie */

    int content_length;      /* body size in bytes (decoded size for chunked); -2 after a failed parse = too large */

    /* Never NULL after success, NUL-terminated, may contain NUL bytes: use content_length, not strlen.
     * On the engine path (parse_http_request_in_place) it points INTO the connection's in_buf - no
     * copy; a chunked body is decoded in place there. parse_http_request / _from_head (tests, tools)
     * copy it into the arena instead. Either way nobody frees it, and handlers must not keep it past
     * their return. */
    char *body;

    /* set by every parse_http_request* to the arena passed in (the one a copied `body` came from). req_get_header
     * and req_get_cookie (on its first call) allocate from it to materialize NUL-terminated strings out
     * of the views above. NULL only for a Request no parse function has ever populated (e.g. a test
     * fixture built and filled by hand without going through parse_http_request*) - req_get_header
     * returns NULL rather than dereference it in that case. */
    Arena *arena;
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

/*
 * One phr_parse_request pass over a raw buffer, cached for reuse (http_parser.h). Before this
 * existed, handle_readable's per-request hot path ran up to four independent phr_parse_request /
 * request_framing passes over the same bytes: the body-limit check, the completeness check, and
 * parse_http_request's own top-level parse plus its internal second request_framing call. Filling one
 * of these once and passing it to parse_http_request_from_head / request_head_is_complete / the body-
 * limit check collapses that to one.
 * header_len == 0 means headers are still incomplete (or malformed - distinguished only by the
 * function's own return value, mirroring request_framing's pre-existing contract); every other field
 * is then not meaningful. method/path point into the buffer that was parsed, not a copy - valid only
 * as long as that buffer is unchanged.
 */
typedef struct {
    size_t header_len;
    const char *method;
    size_t method_len;
    const char *path;
    size_t path_len;
    int minor_version;
    struct phr_header headers[MAX_FRAMING_HEADERS];
    size_t num_headers;
    int content_length;      /* request_framing's code: 0 absent/zero, >0 value, -1 malformed/conflicting, -2 oversized */
    int chunked;
} ParsedHead;

/*
 * Resume point for chunked_body_scan_resume. All offsets are relative to the body start
 * (buf + header_len), never pointers, so they survive in_buf being realloc'd between reads.
 * Zero-initialized = "scan from the start of the body". Only ever advanced past chunks that were
 * fully received and validated, so resuming is exactly equivalent to rescanning from zero.
 *   pos          start of the first chunk-size line not yet fully consumed
 *   decoded_len  sum of the chunk sizes before pos
 *   trailer_from once pos is at the last-chunk ("0") line: where the search for the trailer's
 *                terminating "\r\n\r\n" resumes (bytes before it are known not to start one)
 *   body_end     set when the scan returns 1: offset just past that "\r\n\r\n", i.e. the chunked
 *                body's full wire length - where a pipelined next request starts
 */
typedef struct {
    size_t pos;
    size_t decoded_len;
    size_t trailer_from;
    size_t body_end;
} ChunkScanState;

/* ---- producer streaming (res_stream) ---- */

/*
 * Where a StreamProducer writes one turn's output: a view over Connection.stream_buf, built by
 * flush_connection on the stack for each producer call. Fill it only through stream_write
 * (response.h), which adds the chunked framing. `len` bytes are used of `cap`; `cap` already
 * excludes the reserve for the terminating "0\r\n\r\n", so STREAM_END always fits.
 */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} StreamWriter;

/* StreamProducer return values. */
#define STREAM_MORE 0    /* call again as soon as this turn's output has drained to the socket */
#define STREAM_PAUSE 1   /* nothing more for now: park until app_wake_streams or the next idle sweep (~1 s) */
#define STREAM_END 2     /* finished: the engine sends the last chunk and the connection continues (keep-alive) */
#define STREAM_ABORT (-1) /* failed mid-response: close without the last chunk, so the client sees a truncated body */

/* Called by the event loop, never inside the handler, whenever the previous output has drained:
 * write at most one buffer (STREAM_CHUNK_SIZE) of chunks with stream_write, then return a STREAM_*
 * value. `ctx` is what res_stream was given. STREAM_MORE with nothing written counts as STREAM_PAUSE. */
typedef int (*StreamProducer)(StreamWriter *out, void *ctx);

/* Frees a res_stream ctx. The engine calls it exactly once: after STREAM_END or STREAM_ABORT, when the
 * connection closes for any other reason, when a later res_* replaces the stream, or at once for HEAD. */
typedef void (*StreamCtxFree)(void *ctx);

/* ---- connection and event loop ---- */

struct EventLoopOps; /* defined in event_loop_backend.h (private to the Linux event loop) */

/*
 * io_uring backend: what event_loop_io_uring.c has registered for one fd. Polls are one-shot and
 * re-armed after every event, which gives the level-triggered readiness the engine assumes everywhere
 * (multishot polls fire on wakeups, i.e. edge-triggered). Every arm gets a new `gen`, encoded with the
 * fd in the poll's user_data, so a completion from a poll that has since been removed or replaced
 * (the kernel's -ECANCELED for a removed poll, or readiness it reported just before) is recognized as
 * stale and dropped instead of being reported as an error or delivered to whatever now owns the fd.
 * `mask` is the wanted POLLIN/POLLOUT interest (0 = none); an unchanged mask costs nothing.
 * `armed` is 1 while a poll for `gen` is in flight.
 */
typedef struct {
    uint32_t gen;
    uint16_t mask;
    uint16_t armed;
} PollRegistration;

/* Per-connection state, one per accepted fd, owned by App.connections[fd]. Freed only by connection_close. */
typedef struct Connection {
    int fd;
    int keep_alive;

    time_t last_activity;   /* last successful recv (or accept); writes do not refresh it */

    /* Non-zero while a request is in flight: set on the first byte of a request (handle_readable),
     * cleared once a keep-alive response is fully queued. Unlike last_activity it is never refreshed
     * by a byte arriving, so close_idle_connections can bound total request time even for a client
     * that dribbles one byte at a time (see REQUEST_HEADER_TIMEOUT_SECONDS / REQUEST_BODY_TIMEOUT_SECONDS).
     * Zero means "idle between requests" (or freshly accepted and still silent): only
     * IDLE_TIMEOUT_SECONDS applies then, same as before this field existed. */
    time_t request_started;

    /* Set once per request the first time its headers are found complete (any outcome: within
     * limit, over it, or framing not yet knowable) so handle_readable's per-route body-limit check
     * (app_use_body_limit) runs request_framing at most once per request instead of on every
     * read while a body is still arriving. Cleared with request_started when a keep-alive response
     * is fully queued (flush_connection) so the next request on the same connection is rechecked. */
    int body_limit_checked;

    /* set once "HTTP/1.1 100 Continue" has been attempted for the current request (sent, or
     * skipped on EAGAIN), so a request whose body arrives over many reads gets at most one. Cleared
     * with body_limit_checked when a keep-alive response is fully queued (flush_connection). */
    int continue_sent;

    /* where handle_readable's completeness check resumes scanning a chunked body on the next read,
     * so each body byte is scanned once per request instead of once per recv (was quadratic). Zeroed
     * by connection_create's calloc and again, with body_limit_checked, when a keep-alive response is
     * fully queued. Offsets only (see ChunkScanState), so in_buf growth never invalidates it. */
    ChunkScanState chunk_scan;

    /* Input: NULL (in_cap == 0) whenever nothing is buffered - an idle connection owns no input
     * memory. handle_readable borrows the worker's shared App.read_buf (BUF_SIZE) for the recv and
     * parses complete requests in place there. Before handle_readable returns with the connection
     * still open, unserved bytes (a partial request, or pipelined requests behind a pending
     * response) are copied into a connection-owned malloc'd BUF_SIZE buffer, so in_buf never points
     * at App.read_buf between event-loop turns. An owned buffer grows by doubling (like the chunked
     * path) up to header_len + content_length + 1 to fit a declared body - never straight to the full
     * declared size in one step (a client that declares a large Content-Length and sends little
     * of it costs a reservation proportional to what arrived, not to what it claims) - and is freed
     * by flush_connection once nothing is buffered, or by connection_close. in_buf[in_len] is always
     * '\0' while in_buf != NULL. */
    char *in_buf;
    size_t in_cap;
    size_t in_len;

    /* Pipelining: in_buf may hold more than one request. in_off is where the request being
     * served (or next to be served) starts; request_len is the wire length (headers + framed body)
     * of the request just dispatched, set before its flush_connection. When that response is fully
     * queued on a keep-alive connection, in_off advances by request_len instead of in_buf being
     * discarded. in_off > 0 only while a response is pending or the serve loop yielded
     * (MAX_PIPELINED_PER_EVENT): the leftover is moved to the front of in_buf once, when more input
     * must be read, so every byte moves at most once. Every in_buf parse starts at in_buf + in_off. */
    size_t in_off;
    size_t request_len;

    /* Output: one response built by res_send / res_json / res_write into memory from `arena` (a
     * pointer to the single shared per-worker arena, not a per-connection one - see `arena` below).
     * out_buf != NULL means a response is pending. For responses built by the response layer,
     * out_buf[out_len] == '\0' (not sent).
     * Ownership: normally arena-resident (freed implicitly by the next arena_reset, same as
     * before) - `out_buf_owned` is 0. If a response can't be fully written in one `flush_connection`
     * call (EAGAIN), the unsent tail is copied into a connection-owned `malloc`'d buffer before
     * `flush_connection` returns, since the shared arena would otherwise be reset and reused by
     * another connection before this one's write finishes; `out_buf_owned` becomes 1 and that copy
     * is `free`'d once fully drained or on close. File and producer streaming never touch
     * `out_buf_owned` - see `stream_buf` below. */
    char *out_buf;
    size_t out_len;
    size_t out_sent;
    size_t out_cap;
    int out_buf_owned;

    /* Non-zero while a response is pending (out_buf != NULL, file_fd >= 0 or stream_fn != NULL): set by flush_connection
     * the first time it runs for this response, and advanced only when a write() actually accepts
     * bytes onto the socket - never merely because flush_connection ran. Lets
     * close_idle_connections bound "made no progress at all" separately from "still slowly draining"
     * (WRITE_TIMEOUT_SECONDS), closing a client that stopped reading instead of holding the connection,
     * its arena and out_buf forever. Reset to 0 once a keep-alive response is fully queued. */
    time_t last_write_progress;

    /* File streaming (res_send_file): >= 0 while flush_connection streams file_remaining bytes from disk. */
    int file_fd;
    size_t file_remaining;

    /* Producer streaming (res_stream): stream_fn != NULL from res_stream until STREAM_END/STREAM_ABORT
     * or close. flush_connection calls stream_fn(writer over stream_buf, stream_ctx) each time out_buf has
     * drained, so memory is one STREAM_CHUNK_SIZE buffer however long the response. stream_paused: the
     * producer returned STREAM_PAUSE and everything it wrote has drained - write interest is dropped and
     * read interest kept (only to notice the peer closing) until app_wake_streams or close_idle_connections
     * re-arms write interest. stream_ctx is released through stream_ctx_free exactly once (stream_release,
     * response.c). File and producer streaming are mutually exclusive: a response is at most one of them. */
    StreamProducer stream_fn;
    void *stream_ctx;
    StreamCtxFree stream_ctx_free;
    int stream_paused;

    /* `stream_buf` is a lazily malloc'd, connection-owned STREAM_CHUNK_SIZE buffer shared by
     * both streaming kinds: flush_connection reads each file chunk into it, or has the producer fill it,
     * and reuses it across turns (`out_buf` points at it while streaming) - never the shared arena, since
     * a stream spans many event-loop turns during which other connections would otherwise reuse and
     * overwrite it. Freed when streaming ends (success or error) or on connection_close. */
    char *stream_buf;

    int events_watched;     /* EVENT_READ | EVENT_WRITE currently registered with the event loop */

    /* Per-request bump allocator: a pointer to the single arena shared by every connection this
     * worker process serves (`App.arena`), not one embedded per connection - set once, at
     * connection_create, and never reassigned. Safe because the event loop is single-threaded and
     * non-blocking: at most one connection's handler code runs at a time, and control returns to the
     * event loop (where the next connection's turn may begin) only after that connection's response has
     * either been fully queued or had its still-pending tail copied out to a connection-owned buffer
     * (`out_buf_owned` above) - so nothing any connection still needs is ever left in the shared arena
     * when another connection's turn starts. `arena_reset` runs once per dispatch cycle, in the two
     * places that just finished one (`reject_request`, `handle_readable`'s post-dispatch flush) - not
     * inside `flush_connection` itself, since it also runs on a later, unrelated write-readiness turn
     * where nothing needs resetting again. */
    Arena *arena;
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

/* app_use_body_limit slot: a declared Content-Length over max_bytes for a request whose path
 * falls under prefix (same segment-boundary rule as MiddlewareEntry) is rejected with 413 as soon as
 * headers are complete, before any body buffering. max_bytes is clamped to MAX_BODY_SIZE at
 * registration (it can only tighten the global cap, never loosen it). Chunked bodies are unaffected:
 * they stay governed by the global MAX_BODY_SIZE raw-wire cap only. */
typedef struct {
    char prefix[128];
    size_t max_bytes;
} BodyLimitEntry;

typedef struct {
    char method[8];
    char path[256];               /* pattern, e.g. "/users/:id" */
    Handler handler;              /* NULL for a static-file mount (see static_root) */
    Middleware middlewares[MAX_ROUTE_MIDDLEWARES];
    int middleware_count;
    char *static_root;            /* NULL for an ordinary route; for an app_serve_static mount, the malloc'd
                                   * canonical directory, owned by this Route and freed with it (router.c:
                                   * route_free). Kept out of line: an inline PATH_MAX array made every
                                   * Route, and every Router's Route[MAX_ROUTER_ROUTES], ~4 KB/route bigger on Linux. */
    int has_params;               /* 1 if path has a ':name' segment (match_route then fills req params
                                   * from THIS pattern, never from the shared tree node's name) */
} Route;

typedef enum {
    NODE_STATIC,
    NODE_PARAM,
    NODE_CATCH_ALL
} NodeType;

typedef struct PatriciaNode {
    char *prefix;
    int prefix_len;
    NodeType type;
    Route *route;     /* Pointer to the route data. malloc'd when inserted. NULL if internal node. */
    struct PatriciaNode **children;
    int child_count;
    int child_cap;
    struct PatriciaNode *param_child;
    struct PatriciaNode *catch_all_child;
} PatriciaNode;

typedef struct {
    char method[8];
    PatriciaNode *tree;
} MethodTree;

/* A standalone route table, copied into an App by app_mount. It has no life at dispatch time. */
typedef struct {
    Route routes[MAX_ROUTER_ROUTES];
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

    /* Per-worker cap on concurrently open connections (accepted but not yet closed). Beyond it,
     * accept_connections (connection.c) still accept()s the fd - it has to, to say anything at all -
     * but answers 503 + Connection: close immediately and closes it, without allocating a Connection
     * or touching the event loop. app_init sets DEFAULT_MAX_CONNECTIONS; set to 0 to opt out
     * (uncapped, bounded only by RLIMIT_NOFILE, the old behavior). */
    int max_connections;
} ServerConfig;

/* The whole server. About 4.6 KB on macOS, routes live on the heap: a local or static App is fine. A
 * Router is much larger (64 routes, about 88 KB on macOS): make it static if it is big. Create with
 * app_init, end with app_destroy. */
typedef struct {
    ServerConfig config;
    MethodTree method_trees[16];
    int method_tree_count;
    MiddlewareEntry middlewares[MAX_MIDDLEWARES];
    int middleware_count;

    /* app_use_body_limit registrations, checked by connection.c before a declared Content-Length
     * body is buffered. Unrelated to `middlewares` above: looked up directly by path prefix, not run
     * as part of the middleware chain (it has to be checked before a Request even exists). */
    BodyLimitEntry body_limits[MAX_BODY_LIMITS];
    int body_limit_count;

    ErrorHandler error_handler;
    int server_fd;
    /* CEXPRESS_SINGLE_ACCEPTOR only: 0 = server_fd is a real listen socket, accept() directly
     * (the default, and the only mode on Linux). 1 = server_fd is this worker's control socket (the
     * worker-side end of a socketpair with the cluster master); new connections arrive as passed fds
     * via recvmsg/SCM_RIGHTS (connection.c: accept_passed_connections), never accept()ed by this
     * process. Set once by app_listen_worker_via_control_socket, before app_listen_worker's event loop
     * starts; never toggled afterward. */
    int accept_via_fd_passing;
    /* Which member is live depends on the backend: never test loop_fd on io_uring (it would read half
     * of the ring pointer) - use event_loop_is_open (event_loop.h). */
    union {
        int kq;          /* macOS/BSD */
        int epoll_fd;    /* Linux epoll backend (io_uring fallback, CEXPRESS_EVENT_LOOP=epoll, or NO_URING=1) */
        void *ring;      /* Linux io_uring (struct io_uring*) */
        int loop_fd;     /* platform-neutral int name */
    };
    /* Linux (and the macOS epoll-shim build): the backend event_loop_init chose (event_loop_backend.h),
     * which tells which union member above is live. NULL while no loop is open; kqueue builds leave it NULL. */
    const struct EventLoopOps *loop_ops;
    /* io_uring backend only: the live poll registration of each fd, indexed by fd, grown on demand
     * by event_loop_io_uring.c and freed by event_loop_close. NULL/0 on the other backends. */
    PollRegistration *poll_regs;
    int poll_regs_cap;
    int timer_idle_fd;
    int timer_shutdown_fd;
    int signal_fd;

    /* Indexed directly by fd: the Connection* for an open fd, else NULL. Heap-allocated by app_init,
     * grown by doubling in accept_connections, freed by app_destroy. */
    Connection **connections;
    int connections_cap;

    /* Running count of open connections (accepted, not yet connection_close'd), kept in step with
     * accept_connections (++) and connection_close (--) so the max_connections check in
     * accept_connections is O(1) instead of rescanning `connections` on every accept - which would
     * turn a connection flood into the same O(n) hot-path problem this check exists to guard
     * against. Precise only across that accept/close pairing (the only one production code uses);
     * app_count_connections (a full rescan, called rarely - only at shutdown) remains the
     * authoritative count for anything that walks `connections` directly, e.g. a test harness that
     * pokes it by hand. */
    int open_connections;

    /* One fd (opened once, up front) held in reserve and not otherwise used: on EMFILE/ENFILE from
     * accept() (connection.c: accept_connections), closing it frees exactly one descriptor, just
     * enough to accept the connection that couldn't otherwise be accepted, answer 503, and close it
     * again - the alternative is accept() failing forever with no way to even say the server is
     * full, since every syscall including a rejection's accept() needs a free descriptor. -1 if the
     * initial open failed (degrades to the old silent behavior on EMFILE, same as before this
     * field existed). Opened by app_init, closed by app_destroy. */
    int spare_fd;

    WorkerInitHook worker_init_hooks[MAX_WORKER_INIT_HOOKS];
    int worker_init_hook_count;

    int is_shutting_down;  /* set by app_stop: no new connections, responses carry Connection: close */

    /* Per-request bump allocator, shared by every connection this worker process serves - not one
     * per connection. `app_init` mallocs its ARENA_SIZE (64 KiB) buffer and calls arena_init; every
     * `Connection.arena` this process creates is simply `&app->arena`. Safe under the single-threaded,
     * non-blocking event loop model (see `Connection.arena`'s own comment for why); freed by
     * `app_destroy` (`arena_destroy` plus a `free` of the buffer itself, the same "test/owner frees what
     * it mallocs" convention arena.h documents for a hand-built Arena). */
    Arena arena;

    /* the receive buffer (BUF_SIZE) a connection with nothing buffered borrows in handle_readable
     * - one per worker process, not per connection. Only ever lent for the duration of one
     * handle_readable call (see Connection.in_buf), so a single buffer is enough under the
     * single-threaded event loop. malloc'd by app_init, freed by app_destroy. */
    char *read_buf;
} App;

#endif /* APP_TYPES_H */
