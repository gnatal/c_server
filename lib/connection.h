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
void handle_readable(App *app, Connection *conn);
void handle_writable(App *app, Connection *conn);
void close_idle_connections(App *app);

#endif /* CONNECTION_H */
