#ifndef CONNECTION_H
#define CONNECTION_H

#include <stdint.h>
#include "app_types.h"
#include "event_loop.h"
#include "arena.h"

/*
 * Server lifecycle. One process runs one single-threaded, non-blocking event loop (kqueue on
 * macOS/BSD, epoll on Linux; io_uring readiness polling with CEXPRESS_EVENT_LOOP=io_uring). Typical main():
 *   App app; app_init(&app); ...register middleware and routes...; app_listen(&app, port); app_destroy(&app);
 */

/* Runs the server until SIGINT/SIGTERM, then drains and returns. workers > 1 (or 0 = one per CPU)
 * forks a cluster of SO_REUSEPORT workers under a supervising master (see cluster.h); the master
 * respawns crashed workers. The first signal starts a graceful drain (SHUTDOWN_TIMEOUT_SECONDS),
 * a second forces exit. */
void app_listen(App *app, int port);

/* Same loop without cluster delegation: what app_listen runs in the standalone case and inside each
 * forked worker. Runs the app_on_worker_start hooks first, ignores SIGPIPE, binds - unless
 * app->accept_via_fd_passing is already set (CEXPRESS_SINGLE_ACCEPTOR only), in which case
 * app->server_fd is left as-is (a control socket set by app_listen_worker_via_control_socket, not a
 * listen socket) and this never binds anything itself. */
void app_listen_worker(App *app, int port);

/* CEXPRESS_SINGLE_ACCEPTOR only: entry point for a cluster worker under the single-acceptor
 * model (cluster.c: spawn_worker), where the master - not this process - owns the real listen
 * socket. Sets app->server_fd to control_fd (the worker-side end of this worker's socketpair with
 * the master) and app->accept_via_fd_passing, then runs app_listen_worker's event loop unchanged;
 * new connections arrive as fds the master passes over control_fd (accept_passed_connections) rather
 * than through accept(). port is unused. */
void app_listen_worker_via_control_socket(App *app, int control_fd);

/* Forces a cluster of num_workers processes regardless of app->config.workers. */
void app_listen_cluster(App *app, int port, int num_workers);

/*
 * Registers `hook` to run once in every serving process, at the top of app_listen_worker, always
 * AFTER fork. Use it to open anything with OS-level state (database handle, socket, thread): a
 * resource opened in main() is duplicated into every forked worker, which is unsafe for e.g. SQLite.
 * Hooks run in registration order; at most MAX_WORKER_INIT_HOOKS (extra are dropped with a warning).
 */
void app_on_worker_start(App *app, WorkerInitHook hook);

/* Closes every open connection, the event loop and the listen socket, frees app->connections
 * (the match for app_init's calloc). Safe on an App that never listened. */
void app_destroy(App *app);

/* Begins graceful shutdown: stop accepting, close idle keep-alive connections at once, mark
 * in-flight ones Connection: close, arm the shutdown deadline. Idempotent. */
void app_stop(App *app);

/* Number of open client connections. */
int app_count_connections(const App *app);

/* resumes every res_stream producer on this worker that returned STREAM_PAUSE; each is called
 * again on its next write-readiness event (never from inside this call, so it is safe from a handler,
 * e.g. a POST that publishes to server-sent-event subscribers). Paused producers are also resumed by
 * the idle sweep about once a second without this. Only this process's connections: workers share
 * nothing, so a cluster needs its own cross-worker signal. */
void app_wake_streams(App *app);

/*
 * Deferred responses: a handler that must wait on I/O (a database, another service) without blocking the worker.
 *
 *   static void get_user(const Request *req, Response *res) {
 *       const DeferHandle h = res_defer(res);
 *       if (h == 0) return;                    // refused: 503 already built (over budget), or already answered
 *       start_query(req_get_param(req, "id"), h); // your code keeps h, e.g. in a FIFO of pending queries
 *   }
 *   // later, from an app_watch_fd callback (or any code on this worker's event loop):
 *   Response *res = res_resume(app, h);
 *   if (res != NULL) res_json(res, body);      // NULL: the request is gone (client left, deadline, shutdown)
 *
 * res_defer: the response will be produced after the handler returns. Everything the handler allocated from
 *   res->conn->arena stays valid until the response is written (it takes over the worker arena), and so do the
 *   headers, cookies and status already set on res. Call it before creating a yyjson_alc you will use after
 *   resuming (make it from res->conn->arena after res_resume instead): an allocator made earlier keeps
 *   allocating from the worker arena, which is reset after the handler. Returns 0 without changing anything
 *   when a response was already started, the request is already deferred or the Connection is not the
 *   engine's; returns 0 with a 503 already built when the worker is over ServerConfig.max_buffered_bytes
 *   (the deferred request is charged ARENA_SIZE + its bytes) or out of memory. The handler may still answer
 *   itself after deferring (res_resume inside the handler, or any res_* send): it is then written at once.
 *   While deferred, requests pipelined behind it wait (answered in order afterwards), nothing is read, a
 *   client hang-up closes the connection, and after DEFER_TIMEOUT_SECONDS the client gets 504 and the
 *   connection closes. Middleware code after chain_next runs when the handler returns, not when it resumes.
 * res_resume: the deferred Response, ready for res_status / res_set_header / any sending call. It is written
 *   right after the current event-loop callback returns (never from inside this call, so it is safe from
 *   another connection's handler or a watch callback). A resumed response that sends nothing is answered 500.
 *   Valid until control returns to the event loop. NULL when h is stale: the request was already answered,
 *   its client disconnected, it timed out (504) or the worker drained - drop the result then. Calling it twice
 *   returns the same Response until it is written.
 * req_deferred: the deferred Request (params, query, headers, body), kept until the response is written; NULL
 *   when h is stale, and while the handler that deferred is still running (use its own req there).
 */
DeferHandle res_defer(Response *res);
Response *res_resume(App *app, DeferHandle h);
const Request *req_deferred(App *app, DeferHandle h);

/*
 * Watches an application-owned fd (a database socket, a pipe) on this worker's event loop: on_ready(app, fd,
 * ready, udata) runs when it is readable (WATCH_READ) / writable (WATCH_WRITE), with WATCH_ERROR added on an
 * error or hang-up. events is WATCH_READ, WATCH_WRITE or both; calling again for the same fd replaces the
 * callback and changes the interest (no syscall when it is unchanged), e.g. add WATCH_WRITE while a client
 * library has unsent output. Readiness is level-triggered: it fires again while the condition holds, so after
 * WATCH_ERROR, unwatch the fd (it keeps firing, or on io_uring stops firing). on_ready may be called
 * spuriously: the fd must be non-blocking. Callable from a worker-start hook (applied once the loop starts) or
 * from any handler or callback. -1 for a negative fd, a NULL callback, other bits in events, the listen socket,
 * an fd that is a client connection, or a backend failure.
 * app_unwatch_fd: stop watching (call it before closing the fd). -1 if fd is not watched. Per worker process.
 */
int app_watch_fd(App *app, int fd, unsigned events, FdReadyFn on_ready, void *udata);
int app_unwatch_fd(App *app, int fd);

/*
 * One event-loop turn: waits up to timeout_ms (-1 = until something happens) for events, handles each, and after
 * each one writes the deferred responses it resumed (serve_resumed). Returns the number of events, -1 on a poll
 * error (errno set; EINTR means poll again), or LOOP_TURN_EXIT when the worker should stop serving (second
 * signal, drain deadline, or draining finished). app_listen_worker is a loop over it; tests drive it directly.
 */
#define LOOP_TURN_EXIT (-2)
int app_run_once(App *app, int timeout_ms);

/*
 * Engine internals (called from the event loop; exposed for tests).
 *
 * handle_readable: recv() into conn->in_buf until EAGAIN (into the worker's shared App.read_buf
 *   when nothing is buffered; unserved bytes are copied into a connection-owned buffer before it
 *   returns, so an idle connection owns no input memory). After each recv it serves every complete
 *   request in the buffer, in order (pipelining - parse, route, dispatch, flush_connection, then
 *   the next one from conn->in_off), up to MAX_PIPELINED_PER_EVENT per event. Rejects with 431 (headers
 *   over BUF_SIZE), 414 (path over 255), 413 (body over MAX_BODY_SIZE or an app_use_body_limit prefix),
 *   400 (malformed), 501 (unsupported Transfer-Encoding), 500 (OOM growing the buffer); each
 *   rejection closes the connection, dropping anything pipelined behind it. in_buf grows to fit a
 *   declared body and is freed once nothing is buffered. While a response is
 *   pending it reads nothing (read interest is dropped until the response drains).
 * handle_writable: the LOOP_EVENT_WRITE handler. Continues a pending response via
 *   flush_connection; once none is pending, serves any pipelined requests still buffered (also the
 *   wakeup used when handle_readable hit MAX_PIPELINED_PER_EVENT).
 * flush_connection: non-blocking write of conn->out_buf (and a streamed file or res_stream producer
 *   output, 64 KB per turn; a producer that returns STREAM_PAUSE is parked: FLUSH_PENDING with write
 *   interest dropped, read interest kept only to notice the peer closing).
 *   On EAGAIN it waits for writability and stops reading. When done: keep-alive resets the connection
 *   for the next request (advancing conn->in_off past conn->request_len bytes; request_len 0 consumes
 *   nothing), otherwise it closes. Responses coalesced in App.batch_buf by the serve loop go out first,
 *   in the same write. Returns FLUSH_DONE (fully queued, connection kept),
 *   FLUSH_PENDING (waiting for write readiness) or FLUSH_CLOSED (conn has been freed: don't touch it).
 * close_idle_connections: run once per second; a connection with no received bytes for
 *   IDLE_TIMEOUT_SECONDS (60) is closed (408 first if a request was half-received). A connection
 *   with a response still being written is closed only if it made no write progress for
 *   WRITE_TIMEOUT_SECONDS; a parked res_stream producer is resumed instead.
 * accept_connections: accepts every pending client (accept_client: non-blocking, TCP_NODELAY) and registers it.
 * accept_passed_connections (CEXPRESS_SINGLE_ACCEPTOR only): the fd-passing counterpart - drains
 *   every fd the cluster master has handed this worker over its control socket (server_fd) via
 *   SCM_RIGHTS and registers each one the same way. Never called unless app->accept_via_fd_passing
 *   is set.
 * connection_create / connection_close: one Connection per fd, freed exactly once by connection_close.
 *   connection_create points its arena at the App's single shared per-worker arena rather than
 *   allocating one of its own.
 */
int set_nonblocking(int fd);
/* create_server_socket: listens on bind_address:port. bind_address is a numeric IPv4 or IPv6 literal
 * ("127.0.0.1", "10.0.0.5", "::1", "::" = every interface, IPv4 included), or NULL = every IPv4
 * interface. -1 on failure (a hostname or malformed address included); does not exit() the process
 * itself. The listener is non-blocking with TCP_NODELAY (inherited by accepted sockets) */
int create_server_socket(const char *bind_address, int port);
/* accept_client: one syscall per connection - accept4(SOCK_NONBLOCK | SOCK_CLOEXEC) on Linux,
 * plain accept() on BSD/macOS where the listener's O_NONBLOCK carries over. The returned fd is
 * non-blocking with TCP_NODELAY when listen_fd came from create_server_socket. -1 with accept's
 * errno (EAGAIN when drained, EMFILE/ENFILE when out of descriptors). */
int accept_client(int listen_fd);
Connection *connection_create(App *app, int fd);
void connection_close(App *app, Connection *conn);
void accept_connections(App *app);
void accept_passed_connections(App *app);
#define FLUSH_DONE 0
#define FLUSH_PENDING 1
#define FLUSH_CLOSED -1
int flush_connection(App *app, Connection *conn);
/* serve_resumed: writes every deferred response res_resume marked ready since the last call, then serves
 *   the requests pipelined behind each. Called by app_run_once after each event. */
void serve_resumed(App *app);
void handle_readable(App *app, Connection *conn);
void handle_writable(App *app, Connection *conn);
void close_idle_connections(App *app);

#endif /* CONNECTION_H */
