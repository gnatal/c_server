#ifndef CONNECTION_H
#define CONNECTION_H

#include <stdint.h>
#include "app_types.h"

/* Puts a socket into non-blocking mode so recv()/send()/accept() never stall the single thread. */
int set_nonblocking(int fd);

/* Creates, binds and starts listening on a non-blocking TCP socket for the given port. */
int create_server_socket(int port);

/* Allocates and zero-initializes per-connection state for a freshly accepted fd. */
Connection *connection_create(int fd);

/* Deregisters a connection from kqueue, closes its socket and frees its state. */
void connection_close(App *app, Connection *conn);

/*
 * Tears down everything app_init allocated/opened: closes every still-open
 * connection (via connection_close), closes kq/server_fd if valid, and frees
 * app->connections - the documented match for app_init's calloc (app_types.h:
 * App.connections), per this engine's memory-lifecycle convention. Not called
 * from app_listen's event loop (it never returns there today); intended for
 * test teardown and for a future graceful-shutdown path (pending.txt) to
 * build on. Safe to call on an App that's already been through app_init but
 * never app_listen (server_fd/kq still -1, connections_cap slots all NULL).
 */
void app_destroy(App *app);

/* Thin wrapper around EV_SET + kevent() for registering interest in one filter on one fd. */
void kq_watch(int kq, int fd, int16_t filter, void *udata);

/* Drops interest in one filter on one fd. */
void kq_unwatch(int kq, int fd, int16_t filter);

/*
 * Accepts every pending connection on the listening socket (kqueue is
 * level-triggered, so more than one can be waiting at once), and registers
 * each new client fd for read readiness.
 */
void accept_connections(App *app);

/*
 * Non-blocking write of whatever's left in conn->out_buf. If the socket
 * can't take it all right now, remembers how much was sent and asks kqueue
 * to notify us again once the socket is writable. Once everything is sent:
 * a keep-alive connection is reset and left open for the next request; a
 * "Connection: close" one is torn down.
 */
void flush_connection(App *app, Connection *conn);

/*
 * Non-blocking read of whatever's available on the socket. Once a full
 * request has accumulated in conn->in_buf, parses it, routes it, and runs
 * the app's middleware pipeline (dispatch(), lib/middleware.h) ending at the
 * matched handler, which builds a response via res_send() - then hands off
 * to flush_connection() to actually put it on the wire.
 */
void handle_readable(App *app, Connection *conn);

/*
 * Sweeps app->connections for connections that have gone IDLE_TIMEOUT_SECONDS
 * without the server receiving any bytes (Connection.last_activity) - the
 * Slowloris-shaped gap where a client trickles a request in too slowly, or
 * never sends a next request on a keep-alive connection, without ever
 * tripping the hard buffer-size limits in handle_readable(). A connection
 * with a write still in flight (conn->out_buf != NULL) is left alone even if
 * its read side is stale - that's a slow-reading client on the response,
 * a different problem this function doesn't try to solve. A timed-out
 * connection with a partial request already buffered (conn->in_len > 0)
 * gets a 408 response before closing; one that's simply idle between
 * requests is closed with no response, same as any other idle keep-alive
 * teardown. Called once per IDLE_SWEEP_INTERVAL_MS from app_listen's event
 * loop, off a dedicated EVFILT_TIMER registration.
 */
void close_idle_connections(App *app);

/*
 * Returns the number of currently open client connections tracked in
 * app->connections.
 */
int app_count_connections(const App *app);

/*
 * Initiates graceful shutdown: stops accepting new connections (unwatches and
 * closes server_fd), closes idle keep-alive connections immediately, sets
 * is_shutting_down = 1 so in-flight requests finish with Connection: close,
 * and arms a oneshot shutdown timeout timer (SHUTDOWN_TIMEOUT_SECONDS) on kqueue.
 * Idempotent: safe to call multiple times.
 */
void app_stop(App *app);

/*
 * Starts listening and runs a single-threaded event loop: kevent() blocks
 * until a socket is ready, then each event is dispatched to the right
 * handler - no blocking syscalls anywhere in this loop, and no threads.
 * Catches SIGINT/SIGTERM via kqueue EVFILT_SIGNAL for graceful shutdown,
 * draining in-flight requests before returning.
 */
void app_listen(App *app, int port);

#endif /* CONNECTION_H */
