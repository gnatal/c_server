#ifndef CONNECTION_H
#define CONNECTION_H

#include <stdint.h>
#include "app_types.h"
#include "event_loop.h"
#include "arena.h"

/*
 * Server lifecycle. One process runs one single-threaded, non-blocking event loop (kqueue on
 * macOS/BSD, io_uring readiness polling on Linux; an epoll backend exists behind CEXPRESS_USE_EPOLL). Typical main():
 *   App app; app_init(&app); ...register middleware and routes...; app_listen(&app, port); app_destroy(&app);
 */

/* Runs the server until SIGINT/SIGTERM, then drains and returns. workers > 1 (or 0 = one per CPU)
 * forks a cluster of SO_REUSEPORT workers under a supervising master (see cluster.h); the master
 * respawns crashed workers. The first signal starts a graceful drain (SHUTDOWN_TIMEOUT_SECONDS),
 * a second forces exit. */
void app_listen(App *app, int port);

/* Same loop without cluster delegation: what app_listen runs in the standalone case and inside each
 * forked worker. Runs the app_on_worker_start hooks first, ignores SIGPIPE, sets up TLS, binds. */
void app_listen_worker(App *app, int port);

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
 * (the match for app_init's calloc) and TLS state. Safe on an App that never listened. */
void app_destroy(App *app);

/* Begins graceful shutdown: stop accepting, close idle keep-alive connections at once, mark
 * in-flight ones Connection: close, arm the shutdown deadline. Idempotent. */
void app_stop(App *app);

/* Number of open client connections. */
int app_count_connections(const App *app);

/*
 * Engine internals (called from the event loop; exposed for tests).
 *
 * handle_readable: recv() into conn->in_buf until EAGAIN. Once request_is_complete, it parses,
 *   routes and dispatches synchronously, then flush_connection. Rejects with 431 (headers over
 *   BUF_SIZE), 414 (path over 255), 413 (body over MAX_BODY_SIZE), 400 (malformed), 500 (OOM growing
 *   the buffer); each rejection closes the connection. in_buf grows to fit a declared body and
 *   shrinks back to BUF_SIZE once the connection is idle.
 * flush_connection: non-blocking write of conn->out_buf (and a streamed file, 64 KB per turn).
 *   On EAGAIN it waits for writability. When done: keep-alive resets the connection, otherwise it closes.
 * close_idle_connections: run once per second; a connection with no received bytes for
 *   IDLE_TIMEOUT_SECONDS (60) is closed (408 first if a request was half-received). A connection
 *   with a response still being written is left alone.
 * accept_connections: accepts every pending client (non-blocking, TCP_NODELAY) and registers it.
 * connection_create / connection_close: one Connection per fd (a single calloc that also holds the 64 KiB
 *   per-request arena), freed exactly once by connection_close.
 */
int set_nonblocking(int fd);
int create_server_socket(int port);
Connection *connection_create(int fd);
void connection_close(App *app, Connection *conn);
void accept_connections(App *app);
void flush_connection(App *app, Connection *conn);
void handle_readable(App *app, Connection *conn);
void close_idle_connections(App *app);

#endif /* CONNECTION_H */
