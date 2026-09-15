#ifndef CONNECTION_H
#define CONNECTION_H

#include <stdint.h>
#include "appTypes.h"

/* Puts a socket into non-blocking mode so recv()/send()/accept() never stall the single thread. */
int set_nonblocking(int fd);

/* Creates, binds and starts listening on a non-blocking TCP socket for the given port. */
int create_server_socket(int port);

/* Allocates and zero-initializes per-connection state for a freshly accepted fd. */
Connection *connection_create(int fd);

/* Deregisters a connection from kqueue, closes its socket and frees its state. */
void connection_close(App *app, Connection *conn);

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
 * request has accumulated in conn->in_buf, parses it, routes it, and lets
 * the handler build a response via res_send() - then hands off to
 * flush_connection() to actually put it on the wire.
 */
void handle_readable(App *app, Connection *conn);

/*
 * Starts listening and runs a single-threaded event loop: kevent() blocks
 * until a socket is ready, then each event is dispatched to the right
 * handler - no blocking syscalls anywhere in this loop, and no threads.
 * This is the same shape as libuv's loop underneath Node.js on macOS.
 */
void app_listen(App *app, int port);

#endif /* CONNECTION_H */
