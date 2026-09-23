#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "connection.h"
#include "cluster.h"
#include "event_loop.h"
#include "http_parser.h"
#include "router.h"
#include "response.h"
#include "middleware.h"

static ssize_t conn_read(Connection *conn, void *buf, size_t count) {
    return recv(conn->fd, buf, count, 0);
}

static ssize_t conn_write(Connection *conn, const void *buf, size_t count) {
    return write(conn->fd, buf, count);
}

int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        perror("fcntl(F_GETFL)");
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl(F_SETFL)");
        return -1;
    }
    return 0;
}

/* S12: returns -1 on any failure (socket already closed first) instead of perror()+exit() - a
 * library function shouldn't unilaterally kill the process it's linked into. The two library-side
 * callers (app_listen_worker below, cluster_listen in cluster.c) are the ones that decide a failure
 * here is fatal and exit() themselves, same convention event_loop_init already uses (see lib/CLAUDE.md,
 * "Workers and fork"/"Return conventions") - the decision just now lives at that outer boundary
 * instead of being forced deep inside socket setup. */
int create_server_socket(int port) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return -1;
    }

    const int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR");
        close(server_fd);
        return -1;
    }

#ifdef SO_REUSEPORT
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEPORT");
    }
#endif

    /* P10: set once here instead of once per accepted connection - accepted sockets inherit it from
     * the listener (MEASURED on macOS 26 and Linux 6.8; asserted by test_connection.c's
     * test_accept_client_socket_options so a platform that stops inheriting it fails a test instead
     * of silently turning Nagle back on). */
    if (setsockopt(server_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0) {
        perror("setsockopt TCP_NODELAY");
        close(server_fd);
        return -1;
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind");
        close(server_fd);
        return -1;
    }

    /* S3: take whichever is larger - BACKLOG is a floor, not a cap. No-op on a system whose
     * SOMAXCONN is already <= BACKLOG (e.g. macOS, where kern.ipc.somaxconn is 128, same as
     * BACKLOG); on a Linux whose SOMAXCONN is raised, this actually widens the pending-accept
     * queue instead of the kernel silently dropping SYNs past 128. */
    const int backlog = BACKLOG > SOMAXCONN ? BACKLOG : SOMAXCONN;
    if (listen(server_fd, backlog) < 0) {
        perror("listen");
        close(server_fd);
        return -1;
    }

    if (set_nonblocking(server_fd) < 0) {
        close(server_fd);
        return -1;
    }

    return server_fd;
}

int accept_client(const int listen_fd) {
#if defined(__linux__)
    /* One syscall: Linux does not inherit O_NONBLOCK across accept(), accept4 sets it (and
     * FD_CLOEXEC) atomically. TCP_NODELAY is inherited from the listener. */
    return accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
    /* BSD/macOS: an accepted socket inherits O_NONBLOCK and TCP_NODELAY from the listening socket
     * (create_server_socket sets both), so plain accept() is already the whole job. */
    return accept(listen_fd, NULL, NULL);
#endif
}

Connection *connection_create(App *app, int fd) {
    Connection *conn = calloc(1, sizeof(Connection));
    if (conn == NULL) {
        return NULL;
    }
    /* M2: in_buf stays NULL (calloc) - input memory is borrowed from App.read_buf per read and only
     * owned while a request is partially received (see Connection.in_buf). */
    conn->fd = fd;
    conn->file_fd = -1;
    conn->last_activity = time(NULL);
    /* request_started stays 0 (calloc) until a request actually starts arriving: a freshly accepted,
     * otherwise-silent connection is bounded by IDLE_TIMEOUT_SECONDS below, same as before (S1 targets
     * a request that is under way but moving too slowly, not one that never starts). */
    conn->arena = &app->arena; /* M1: shared per-worker arena, not one allocated per connection */
    return conn;
}

void connection_close(App *app, Connection *conn) {
    if (conn == NULL) {
        return;
    }
    if (app != NULL) {
        event_loop_unwatch_all(app, conn->fd);
        if (app->connections != NULL && conn->fd >= 0 && conn->fd < app->connections_cap) {
            app->connections[conn->fd] = NULL;
        }
        app->open_connections--; /* paired with accept_connections' ++ (S3) */
        if (app->spare_fd < 0) {
            /* Opportunistic re-arm (S3): this connection closing just freed a descriptor, the
             * cheapest possible moment to try reclaiming the reserve - waiting for the next
             * accept_connections call could be a long time if the listen socket is the thing
             * that's starved. Best effort: still -1 on failure, tried again next close. */
            app->spare_fd = open("/dev/null", O_RDONLY);
        }
    }

    if (conn->file_fd >= 0) {
        close(conn->file_fd);
        conn->file_fd = -1;
    }
    /* M1: file_buf and a still-owned malloc'd out_buf tail-copy are connection-owned, unlike the
     * (shared, App-owned) arena a normal out_buf lives in - free them here regardless of which exit
     * path got the connection closed (a hard write/read error mid-response, not just the ordinary
     * "response fully sent, not keeping this connection alive" case). out_buf can equal file_buf
     * (streaming's out_buf just points at it) - free it once, via file_buf, never both. */
    if (conn->out_buf == conn->file_buf) {
        conn->out_buf = NULL;
    } else if (conn->out_buf_owned) {
        free(conn->out_buf);
        conn->out_buf = NULL;
    }
    free(conn->file_buf);
    conn->file_buf = NULL;

    if (conn->fd >= 0) {
        close(conn->fd);
        conn->fd = -1;
    }
    if (app == NULL || conn->in_buf != app->read_buf) {
        free(conn->in_buf); /* M2: owned (or NULL); App.read_buf is only ever borrowed, app_destroy frees it */
    }
    conn->in_buf = NULL;
    conn->out_buf = NULL;
    free(conn);
}

void app_destroy(App *app) {
    if (app == NULL) {
        return;
    }
    if (app->connections != NULL) {
        for (int fd = 0; fd < app->connections_cap; fd++) {
            if (app->connections[fd] != NULL) {
                connection_close(app, app->connections[fd]);
            }
        }
    }
    app_free_routes(app);
    event_loop_close(app);
    if (app->server_fd >= 0) {
        close(app->server_fd);
        app->server_fd = -1;
    }
    if (app->spare_fd >= 0) {
        close(app->spare_fd);
        app->spare_fd = -1;
    }
    /* M1: the one shared arena every Connection.arena pointed at. Every connection above was already
     * closed (connection_close no longer touches app->arena itself), so nothing still references it. */
    arena_destroy(&app->arena);
    free(app->arena.buf);
    app->arena.buf = NULL;
    free(app->read_buf); /* M2: no connection is left to have borrowed it */
    app->read_buf = NULL;
    free(app->connections);
    app->connections = NULL;
    app->connections_cap = 0;
}

int app_count_connections(const App *app) {
    if (app == NULL || app->connections == NULL) {
        return 0;
    }
    int count = 0;
    for (int fd = 0; fd < app->connections_cap; fd++) {
        if (app->connections[fd] != NULL) {
            count++;
        }
    }
    return count;
}

void app_stop(App *app) {
    if (app == NULL || app->is_shutting_down) {
        return;
    }
    app->is_shutting_down = 1;

    /* Stop accepting new connections: deregister from event loop and close server socket */
    if (app->server_fd >= 0) {
        if (app->loop_fd >= 0) {
            event_loop_unwatch_read(app, app->server_fd);
        }
        close(app->server_fd);
        app->server_fd = -1;
    }

    /* Sweep open connections:
     * - Immediately close idle keep-alive connections (nothing in flight).
     * - For in-flight connections (reading request or writing response), disable
     *   keep_alive so they close as soon as their current response is flushed. */
    if (app->connections != NULL) {
        for (int fd = 0; fd < app->connections_cap; fd++) {
            Connection *conn = app->connections[fd];
            if (conn == NULL) {
                continue;
            }
            if (conn->in_len == 0 && conn->out_buf == NULL && conn->file_fd < 0) {
                connection_close(app, conn);
            } else {
                conn->keep_alive = 0;
            }
        }
    }

    /* Arm a oneshot shutdown deadline timer on the event loop so slow/stalled clients
     * cannot prevent the server process from exiting indefinitely. */
    if (app->loop_fd >= 0) {
        event_loop_arm_shutdown_timer(app);
    }
}

/*
 * Grows app->connections (realloc, doubling) until index fd fits, zeroing
 * the newly added slots (realloc doesn't zero new memory) so an unused fd
 * always reads as NULL - same invariant the initial calloc in app_init
 * establishes. Called from accept_connections before indexing a freshly
 * accepted fd's slot; a failed realloc leaves the original block (and
 * app->connections/connections_cap) untouched at its prior, still-usable
 * size, so this only ever costs the one connection being accepted, not any
 * already-open ones. Returns 0 on success, -1 on failure.
 */
static int ensure_connection_capacity(App *app, int fd) {
    if (fd < app->connections_cap) {
        return 0;
    }
    int new_cap = app->connections_cap;
    while (fd >= new_cap) {
        new_cap *= 2;
    }
    Connection **grown = realloc(app->connections, (size_t)new_cap * sizeof(Connection *));
    if (grown == NULL) {
        return -1;
    }
    memset(grown + app->connections_cap, 0, (size_t)(new_cap - app->connections_cap) * sizeof(Connection *));
    app->connections = grown;
    app->connections_cap = new_cap;
    return 0;
}

/*
 * S3 overload shedding: writes a minimal, hand-built 503 (no Connection/arena/Response - this
 * happens before any of that would normally exist) to a freshly accepted fd, then closes it.
 * Best-effort and one nonblocking attempt only: a client that won't read even this gets no more
 * consideration than one that was never told anything, which is fine - the point is to answer
 * politely when possible, not to guarantee delivery under overload (that would need to hold and
 * retry the write, defeating the purpose of shedding cheaply).
 */
static void reject_overloaded_connection(int client_fd) {
    const char *body = status_text(503);
    char head[160];
    int head_len = snprintf(head, sizeof(head),
                             "HTTP/1.1 503 %s\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n"
                             "Connection: close\r\n\r\n",
                             body, strlen(body));
    /* client_fd is already non-blocking: accept_client, or a master's accept_client before an
     * SCM_RIGHTS handoff (P10 - this used to cost its own two fcntl calls). */
    if (head_len > 0 && (size_t)head_len < sizeof(head)) {
        ssize_t n = write(client_fd, head, (size_t)head_len);
        if (n == head_len) {
            write(client_fd, body, strlen(body));
        }
    }
    close(client_fd);
}

/*
 * Shared tail of admitting one already-obtained client fd (via accept() or, under
 * CEXPRESS_SINGLE_ACCEPTOR, a passed fd received over a control socket - C4): S3's overload check,
 * table growth and Connection setup. Expects O_NONBLOCK/TCP_NODELAY to be in effect already: an fd
 * from accept_client has both (P10: accept4 / inheritance from the listener, no per-fd syscalls); a
 * passed fd was accept_client'ed by the master and keeps both (POSIX: file status flags and socket
 * options are properties of the underlying open file description, not the fd number, so they carry
 * over across an SCM_RIGHTS handoff same as across dup()/fork()).
 */
static void admit_connection(App *app, int client_fd) {
    /* S3: shed load past the configured cap instead of accumulating connections (and their
     * arenas) without bound. Checked before ensure_connection_capacity/connection_create so an
     * overloaded server doesn't pay for either. open_connections is O(1) (see App.open_connections)
     * - this runs once per accepted connection, including every one we're about to reject, so it
     * must not become the O(n) scan it's replacing. */
    if (app->config.max_connections > 0 && app->open_connections >= app->config.max_connections) {
        reject_overloaded_connection(client_fd);
        return;
    }

    /* Past the cap above, fd is bounded only by the process's own RLIMIT_NOFILE (accept()
     * itself starts failing with EMFILE once that's hit, handled by the caller) - no fixed ceiling
     * here, just grow the table to fit. A failed grow (genuine OOM) rejects just this one
     * connection, without affecting any already-open ones. */
    if (ensure_connection_capacity(app, client_fd) != 0) {
        close(client_fd);
        return;
    }

    Connection *conn = connection_create(app, client_fd);
    if (conn == NULL) {
        close(client_fd);
        return;
    }
    app->connections[client_fd] = conn;
    app->open_connections++; /* paired with connection_close's -- (S3) */

    event_loop_watch_read(app, client_fd, conn);
}

void accept_connections(App *app) {
    while (1) {
        const int client_fd = accept_client(app->server_fd);
        if (client_fd < 0) {
            /* Out of descriptors process- or system-wide: without a free slot, accept() keeps
             * failing this way forever (nothing here releases one on its own), silently starving
             * every future connection - the pre-S3 behavior. MEASURED (macOS/BSD): the connection
             * that triggered *this* failure is already gone by the time we see the error - accept()
             * dequeues it off the listen backlog and destroys it when it can't allocate an fd,
             * rather than leaving it there for a retry, so there is nobody left here to answer with
             * a 503 (not verified on Linux; a kernel that instead leaves it queued would make a
             * retry-accept recover it, which is harmless to also attempt, but this codebase doesn't
             * rely on that). What the spare fd can still do is free up exactly one slot so the
             * *next* accept() call - for whatever connection comes after this one, now or on a later
             * call to accept_connections - succeeds instead of the worker staying stuck at the limit
             * indefinitely. connection_close re-arms the spare fd opportunistically once anything
             * closes. */
            if ((errno == EMFILE || errno == ENFILE) && app->spare_fd >= 0) {
                close(app->spare_fd);
                app->spare_fd = -1;
                continue;
            }
            break;
        }

        admit_connection(app, client_fd);
    }
}

#ifdef CEXPRESS_SINGLE_ACCEPTOR
/*
 * C4: this worker's server_fd is a control socket (the worker-side end of a socketpair with the
 * cluster master), not a listen socket - accept_via_fd_passing is set. New connections arrive as fds
 * passed over it via SCM_RIGHTS (cluster.c: dispatch_client_fd), never accept()ed by this process.
 * Drains every pending message until EAGAIN, same loop shape as accept_connections.
 */
void accept_passed_connections(App *app) {
    while (1) {
        char data_buf[1];
        char cmsg_buf[CMSG_SPACE(sizeof(int))];
        struct iovec iov = { .iov_base = data_buf, .iov_len = sizeof(data_buf) };
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cmsg_buf;
        msg.msg_controllen = sizeof(cmsg_buf);

        ssize_t n = recvmsg(app->server_fd, &msg, 0);
        if (n <= 0) {
            /* n == 0: the master closed its end of this control socket (e.g. this worker is
             * draining/exiting) - nothing more to read, same as accept() returning EAGAIN below.
             * n < 0: EAGAIN/EWOULDBLOCK (no more pending hand-offs right now) or a transient error;
             * either way, stop draining this wake. */
            break;
        }

        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        if (cmsg == NULL || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
            /* A message arrived with no fd attached - nothing to admit. Shouldn't happen (the
             * master only ever sends one-byte-plus-fd messages), but harmless to just skip it. */
            continue;
        }

        int client_fd;
        memcpy(&client_fd, CMSG_DATA(cmsg), sizeof(int));
        admit_connection(app, client_fd);
    }
}
#endif

/*
 * P9: a response could not be fully written this turn. Ask for write readiness and stop reading
 * until it drains: in_buf may already hold the next pipelined request, and serving it now would
 * build a new response over the one still pending. Level-triggered read readiness would otherwise
 * keep firing for bytes handle_readable must not consume yet. flush_connection's keep-alive branch
 * re-watches read once the response is fully queued (a closed connection needs neither).
 */
static void wait_for_writable(App *app, Connection *conn) {
    event_loop_watch_write(app, conn->fd, conn);
    if (conn->events_watched & EVENT_READ) {
        event_loop_unwatch_read(app, conn->fd);
    }
}

/* M2: nothing is buffered any more - drop the input memory so an idle connection holds none. An
 * owned buffer is freed; a borrowed App.read_buf is just handed back. */
static void release_in_buf(const App *app, Connection *conn) {
    if (conn->in_buf != app->read_buf) {
        free(conn->in_buf);
    }
    conn->in_buf = NULL;
    conn->in_cap = 0;
    conn->in_len = 0;
    conn->in_off = 0;
}

int flush_connection(App *app, Connection *conn) {
    size_t bytes_written_this_flush = 0;
    const size_t max_flush_bytes = 4 * STREAM_CHUNK_SIZE;

    if (conn->last_write_progress == 0) {
        /* First time flush_connection runs for this response: start the S2 stall clock now, even
         * before the first byte actually goes out (a response that gets EAGAIN on every attempt is
         * exactly the "made no progress" case the deadline exists for). */
        conn->last_write_progress = time(NULL);
    }

    while (1) {
        while (conn->out_sent < conn->out_len) {
            ssize_t n = conn_write(conn, conn->out_buf + conn->out_sent, conn->out_len - conn->out_sent);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* M1: out_buf is shared-arena-resident unless it's already a connection-owned
                     * copy (out_buf_owned) or the file-streaming chunk buffer (out_buf == file_buf,
                     * never arena to begin with - see file_buf's own comment). Returning to the event
                     * loop now would let another connection's dispatch reset and reuse the shared
                     * arena before this response finishes draining, so copy what's left out first. */
                    if (!conn->out_buf_owned && conn->out_buf != conn->file_buf) {
                        size_t remaining = conn->out_len - conn->out_sent;
                        char *tail = malloc(remaining);
                        if (tail == NULL) {
                            connection_close(app, conn);
                            return FLUSH_CLOSED;
                        }
                        memcpy(tail, conn->out_buf + conn->out_sent, remaining);
                        conn->out_buf = tail;
                        conn->out_len = remaining;
                        conn->out_sent = 0;
                        conn->out_cap = remaining;
                        conn->out_buf_owned = 1;
                    }
                    wait_for_writable(app, conn);
                    return FLUSH_PENDING;
                }
                connection_close(app, conn);
                return FLUSH_CLOSED;
            }
            conn->out_sent += (size_t)n;
            conn->last_activity = time(NULL);
            conn->last_write_progress = conn->last_activity; /* S2: only advances on actual bytes written */
            bytes_written_this_flush += (size_t)n;
        }

        /* The current out_buf has been fully drained to the socket */
        if (conn->file_fd >= 0) {
            if (conn->file_remaining > 0) {
                if (bytes_written_this_flush >= max_flush_bytes) {
                    /* Yield to event loop to share bandwidth fairly */
                    wait_for_writable(app, conn);
                    return FLUSH_PENDING;
                }

                size_t to_read = conn->file_remaining < STREAM_CHUNK_SIZE
                                     ? conn->file_remaining
                                     : STREAM_CHUNK_SIZE;
                /* M1: a connection-owned buffer, malloc'd once and reused chunk to chunk - never the
                 * shared arena, since a large file spans many event-loop turns during which other
                 * connections would otherwise reuse and overwrite it (see Connection.file_buf). A
                 * still-owned malloc'd tail-copy from the response head (above) is done with once we
                 * get here (the inner loop just fully drained it) and isn't file_buf, so free it now
                 * rather than leak it when out_buf moves on to file_buf below. */
                if (conn->out_buf_owned) {
                    free(conn->out_buf);
                    conn->out_buf_owned = 0;
                }
                if (conn->file_buf == NULL) {
                    conn->file_buf = malloc(STREAM_CHUNK_SIZE);
                    if (conn->file_buf == NULL) {
                        connection_close(app, conn);
                        return FLUSH_CLOSED;
                    }
                }
                conn->out_buf = conn->file_buf;
                ssize_t r = read(conn->file_fd, conn->out_buf, to_read);
                if (r <= 0) {
                    connection_close(app, conn);
                    return FLUSH_CLOSED;
                }
                conn->out_len = (size_t)r;
                conn->out_sent = 0;
                conn->out_cap = STREAM_CHUNK_SIZE;
                conn->file_remaining -= (size_t)r;
                continue;
            } else {
                close(conn->file_fd);
                conn->file_fd = -1;
                free(conn->file_buf);
                conn->file_buf = NULL;
                conn->out_buf = NULL;
                conn->out_cap = 0;
            }
        }

        break;
    }

    /* Whatever is left in out_buf now has been fully drained (the inner loop only exits via that, or
     * the streaming branch above, which never leaves it non-NULL and un-freed). A connection-owned
     * tail-copy (M1) needs freeing here; an arena-resident buffer does not (the caller - handle_readable
     * or reject_request - resets the shared arena once this whole dispatch-and-flush cycle is done). */
    if (conn->out_buf_owned) {
        free(conn->out_buf);
        conn->out_buf_owned = 0;
    }

    if (conn->keep_alive) {
        /* Drop write registration from a partial write above, and resume reading (P9: reads pause
         * while a response is pending - see wait_for_writable). */
        event_loop_unwatch_write(app, conn->fd, conn);
        if (!(conn->events_watched & EVENT_READ)) {
            event_loop_watch_read(app, conn->fd, conn);
        }
        conn->out_buf = NULL;
        conn->out_len = 0;
        conn->out_sent = 0;
        conn->out_cap = 0;
        /* P9: keep whatever follows the request just answered (a pipelined next request, whole or
         * partial) instead of discarding the buffer. request_len == 0 means nobody recorded how long
         * the request was (a direct flush_connection call): drop everything, the pre-P9 behavior. */
        const size_t consumed = conn->request_len;
        conn->request_len = 0;
        if (consumed == 0 || conn->in_off + consumed >= conn->in_len) {
            conn->in_len = 0;
            conn->in_off = 0;
        } else {
            conn->in_off += consumed;
        }
        /* Back to idle between requests (S1) - unless pipelined bytes are already waiting, which
         * start the next request's clock now. */
        conn->request_started = conn->in_len > 0 ? time(NULL) : 0;
        conn->last_write_progress = 0; /* no response pending: WRITE_TIMEOUT_SECONDS stops applying (S2) */
        conn->body_limit_checked = 0; /* next request on this connection gets its own body-limit check (S4) */
        conn->chunk_scan = (ChunkScanState){0}; /* next request's chunked body scans from its own start (P8) */

        /* M2: nothing buffered - free the input buffer (however far it grew for a large body) or hand
         * App.read_buf back, so an idle keep-alive connection owns no input memory. Pipelined
         * leftovers (P9) keep the buffer as it is. */
        if (conn->in_len == 0) {
            release_in_buf(app, conn);
        }
        return FLUSH_DONE;
    }
    connection_close(app, conn);
    return FLUSH_CLOSED;
}

/* Sends `status` with its reason phrase as the body, then closes the connection. */
static void reject_request(App *app, Connection *conn, const int status) {
    Response res;
    res_init(&res, conn);
    conn->keep_alive = 0;
    res_status(&res, status);
    res_send(&res, status_text(status));
    flush_connection(app, conn);
    /* M1: conn may already be freed (flush_connection always closes here, keep_alive is forced off
     * above) - reset the shared arena through app, never conn, once this dispatch-and-flush cycle
     * that just used it is over. */
    arena_reset(&app->arena);
}

/*
 * in_buf is full with headers complete: grow it to fit the body (M2: a borrowed App.read_buf is
 * copied into a new owned buffer instead of realloc'd). Returns 1 grown (keep reading),
 * 0 realloc failed (-> 500), -1 chunked raw-size cap hit (-> 413).
 * Both Content-Length and chunked grow the same way (S4): doubling, capped at the known target size
 * (header_len + content_length + 1, or header_len + MAX_BODY_SIZE for chunked's raw wire size) - never
 * one realloc straight to the full size a client merely *declared*. A client that sends a 10 MiB
 * Content-Length and then only a few KB of body costs a reservation proportional to what actually
 * arrived (doubling from BUF_SIZE), not the declared 10 MiB up front; the full target is only reached
 * once that much has genuinely been received (and buffered) across repeated calls here.
 * A Content-Length above MAX_BODY_SIZE never gets here: request_is_complete already stopped
 * buffering and parse_http_request answered 413. A route-specific limit below MAX_BODY_SIZE
 * (app_use_body_limit) is enforced earlier still, in handle_readable, before this is ever called.
 */
static int grow_in_buf(const App *app, Connection *conn, const size_t header_len, const int chunked,
                       const int content_length) {
    size_t needed;
    if (chunked) {
        const size_t raw_cap = header_len + MAX_BODY_SIZE;
        if (conn->in_cap >= raw_cap) {
            return -1;
        }
        needed = conn->in_cap * 2 > raw_cap ? raw_cap : conn->in_cap * 2;
    } else if (content_length >= 0) {
        const size_t target = header_len + (size_t)content_length + 1;
        if (conn->in_cap >= target) {
            return 1; /* already large enough (request_is_complete will pick this up next read) */
        }
        needed = conn->in_cap * 2 > target ? target : conn->in_cap * 2;
    } else {
        return 0;
    }
    char *grown;
    if (conn->in_buf == app->read_buf) {
        /* M2: a borrowed App.read_buf is never realloc'd - move to an owned buffer of the grown size. */
        grown = malloc(needed);
        if (grown == NULL) {
            return 0;
        }
        memcpy(grown, conn->in_buf, conn->in_len + 1);
    } else {
        grown = realloc(conn->in_buf, needed);
        if (grown == NULL) {
            return 0;
        }
    }
    conn->in_buf = grown;
    conn->in_cap = needed;
    return 1;
}

/*
 * S4: as soon as a request's headers are complete, reject a declared Content-Length that exceeds
 * the effective app_use_body_limit for its path with 413 - before any body buffering happens, not
 * just before in_buf is grown to fit it. Runs at most once per request (conn->body_limit_checked).
 * Only Content-Length is covered: chunked bodies stay governed by the global MAX_BODY_SIZE raw-wire
 * cap in grow_in_buf/chunked_body_scan (see app_use_body_limit's header comment for why).
 * Takes an already-parsed head (P2) instead of running its own request_framing pass over conn->in_buf.
 * Returns 1 if the request was rejected (caller must not touch conn again), 0 otherwise.
 */
static int reject_if_over_body_limit(App *app, Connection *conn, const ParsedHead *head) {
    if (conn->body_limit_checked) {
        return 0;
    }
    if (head->header_len == 0) {
        return 0; /* headers still incomplete: nothing to check yet, try again next read */
    }
    conn->body_limit_checked = 1;
    if (head->chunked || head->content_length < 0) {
        return 0; /* not this check's job: chunked, absent, or already malformed/oversized globally */
    }
    char path_buf[256];
    const size_t n = head->path_len < sizeof(path_buf) - 1 ? head->path_len : sizeof(path_buf) - 1;
    memcpy(path_buf, head->path, n);
    path_buf[n] = '\0';
    if ((size_t)head->content_length > app_body_limit_for_path(app, path_buf)) {
        reject_request(app, conn, 413);
        return 1;
    }
    return 0;
}

/* serve_buffered_requests results (P9). */
#define SERVE_NEED_MORE 0
#define SERVE_WAIT 1
#define SERVE_CLOSED -1

/* P9: whether a response is still being written (built but not fully queued on the socket). */
static int response_pending(const Connection *conn) {
    return conn->out_buf != NULL || conn->file_fd >= 0;
}

/* P9: moves the unserved tail of in_buf (from in_off) to the front. Called only when more input has
 * to be read after it (serve_buffered_requests' SERVE_NEED_MORE), so every byte moves at most once. */
static void compact_in_buf(Connection *conn) {
    if (conn->in_off == 0) {
        return;
    }
    const size_t leftover = conn->in_len - conn->in_off;
    memmove(conn->in_buf, conn->in_buf + conn->in_off, leftover);
    conn->in_len = leftover;
    conn->in_off = 0;
    conn->in_buf[conn->in_len] = '\0';
}

/*
 * P9: serves every complete request buffered in in_buf[in_off..in_len), in order, one response each,
 * up to MAX_PIPELINED_PER_EVENT per call; *served_out = how many were dispatched. Returns:
 *   SERVE_NEED_MORE  no complete request left (in_buf compacted: any partial request now starts at 0)
 *   SERVE_WAIT       a response is pending (write readiness resumes it) or the per-call cap was hit
 *                    (a write-readiness wakeup is armed to continue - see handle_writable)
 *   SERVE_CLOSED     the connection was closed (rejected, non-keep-alive, or a write error): conn freed
 */
static int serve_buffered_requests(App *app, Connection *conn, int *served_out) {
    *served_out = 0;
    for (int served = 0; served < MAX_PIPELINED_PER_EVENT; served++, (*served_out)++) {
        if (conn->in_off == conn->in_len) {
            conn->in_off = 0;
            conn->in_len = 0;
            return SERVE_NEED_MORE;
        }
        const char *req_start = conn->in_buf + conn->in_off;
        const size_t req_avail = conn->in_len - conn->in_off;

        /* One phr_parse_request pass, reused below by the body-limit check, the completeness check
         * and the full parse (P2) - these used to each run their own independent pass over the same
         * bytes, up to four per request after S4 added the body-limit check's own. */
        ParsedHead head;
        parse_request_head(req_start, req_avail, &head);

        if (reject_if_over_body_limit(app, conn, &head)) {
            return SERVE_CLOSED;
        }

        if (!request_head_is_complete(&head, req_start, req_avail, &conn->chunk_scan)) {
            compact_in_buf(conn);
            return SERVE_NEED_MORE;
        }

        Request req;
        const int parse_status = parse_http_request_from_head(req_start, req_avail, &head, &req, conn->arena);
        if (parse_status != 0) {
            /* -2: path too long (414). -3 (S5) is retired (P3): req->headers holds views now, so
             * there is no fixed-size copy left to overflow - parse_http_request_from_head never
             * returns it any more (this is parse_status's own -3; req.content_length's -3 below is
             * an unrelated sentinel in a different code space). -4: a percent-decoded path/query
             * name/query value contained an embedded NUL, never silently truncated (400, S6) - falls
             * into "anything else" below along with -1 (malformed), since both are already 400.
             * req.content_length == -2: body over MAX_BODY_SIZE (413), for both Content-Length and
             * chunked framing. req.content_length == -3 (S11): Transfer-Encoding names a coding this
             * engine doesn't implement, or "chunked" isn't its sole token (501, "Not Implemented" -
             * the server understood the request but can't process that transfer-coding). Anything
             * else: 400. */
            /* req.body is managed by arena, no need to free */
            /* req.body is managed by arena, no need to free */
            const int status = parse_status == -2 ? 414
                              : req.content_length == -2 ? 413
                              : req.content_length == -3 ? 501
                              : 400;
            reject_request(app, conn, status);
            return SERVE_CLOSED;
        }
        /* Parse succeeded, so framing is valid: this is where the next pipelined request starts. */
        conn->request_len = request_wire_len(&head, &conn->chunk_scan);

        Response res;
        res_init(&res, conn);
        conn->keep_alive = !request_wants_close(&req) && !app->is_shutting_down;
        /* Set before dispatch so HEAD bodies are suppressed for matched routes and 404/405 alike. */
        res.is_head_request = strcmp(req.method, "HEAD") == 0;
        const Route *route = match_route(app, &req);
        dispatch(app, route, &req, &res);
        /* req.body is managed by arena, no need to free */

        const int flushed = flush_connection(app, conn);
        /* M1: conn may already be freed by flush_connection (a non-keep-alive response, or a
         * hard write error) - reset the shared arena through app, never conn, once this
         * dispatch-and-flush cycle that just used it is over (flush_connection has already
         * copied out anywhere it returned early with a still-pending response, so nothing any
         * connection still needs is left in it). */
        arena_reset(&app->arena);
        if (flushed == FLUSH_CLOSED) {
            return SERVE_CLOSED;
        }
        if (flushed == FLUSH_PENDING) {
            return SERVE_WAIT; /* in_off still marks the next request; handle_writable resumes */
        }
    }
    if (conn->in_off == conn->in_len) {
        conn->in_off = 0;
        conn->in_len = 0;
        return SERVE_NEED_MORE;
    }
    /* Cap reached with bytes still buffered: let other connections run, and come back on the next
     * poll via write readiness (a connected socket is almost always writable). Nothing is pending,
     * so read interest stays as it is. */
    event_loop_watch_write(app, conn->fd, conn);
    return SERVE_WAIT;
}

/*
 * M2: handle_readable is returning with conn still open. If in_buf is the borrowed App.read_buf,
 * either hand it back (nothing left to serve) or copy the unserved bytes - a partial request, or
 * requests pipelined behind a pending response - into an owned BUF_SIZE buffer, compacted to offset
 * 0 (request_len and chunk_scan are relative to in_off, so they stay valid). The leftover always
 * fits: it came out of a BUF_SIZE buffer. Returns 0, or -1 (malloc failed, conn closed).
 */
static int stop_borrowing_read_buf(App *app, Connection *conn) {
    if (conn->in_buf != app->read_buf) {
        return 0;
    }
    const size_t leftover = conn->in_len - conn->in_off;
    if (leftover == 0) {
        release_in_buf(app, conn);
        return 0;
    }
    char *owned = malloc(BUF_SIZE);
    if (owned == NULL) {
        connection_close(app, conn); /* leaves the borrowed App.read_buf alone */
        return -1;
    }
    memcpy(owned, conn->in_buf + conn->in_off, leftover);
    owned[leftover] = '\0';
    conn->in_buf = owned;
    conn->in_cap = BUF_SIZE;
    conn->in_len = leftover;
    conn->in_off = 0;
    return 0;
}

/* handle_readable's body. Returns 0 with conn still open, -1 once conn has been closed (freed). */
static int read_and_serve(App *app, Connection *conn) {
    if (response_pending(conn) || conn->in_off > 0) {
        /* P9: a response is still draining (reads are normally unwatched meanwhile, see
         * wait_for_writable - this guards a readiness event already queued in the same batch), or
         * buffered pipelined requests are waiting for their handle_writable turn. Reading now would
         * append behind them; they are served first, in order. */
        if (response_pending(conn)) {
            return 0;
        }
        int served;
        const int status = serve_buffered_requests(app, conn, &served);
        if (status != SERVE_NEED_MORE) {
            return status == SERVE_CLOSED ? -1 : 0;
        }
    }

    if (conn->in_buf == NULL) {
        /* M2: nothing buffered - read into the worker's shared buffer; stop_borrowing_read_buf
         * (handle_readable) moves any unserved bytes into an owned buffer before returning. */
        conn->in_buf = app->read_buf;
        conn->in_cap = BUF_SIZE;
        conn->in_len = 0;
        conn->in_off = 0;
    }

    while (conn->in_len < conn->in_cap - 1) {
        ssize_t n = conn_read(conn, conn->in_buf + conn->in_len, conn->in_cap - 1 - conn->in_len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            connection_close(app, conn);
            return -1;
        }
        if (n == 0) {
            connection_close(app, conn);
            return -1;
        }

        if (conn->request_started == 0) {
            /* First byte of a fresh request after an idle keep-alive gap: (re)start the deadline clock (S1). */
            conn->request_started = time(NULL);
        }
        conn->in_len += (size_t)n;
        conn->in_buf[conn->in_len] = '\0';
        conn->last_activity = time(NULL);

        /* Return as soon as anything was answered, even if more input may follow: another recv()
         * here would almost always just return EAGAIN (one wasted syscall per request - MEASURED ~9%
         * of non-pipelined throughput). Level-triggered read readiness brings us back if more bytes
         * are already waiting. */
        int served;
        const int status = serve_buffered_requests(app, conn, &served);
        if (status == SERVE_CLOSED) {
            return -1;
        }
        if (status != SERVE_NEED_MORE || served > 0) {
            return 0;
        }
    }

    if (conn->in_len >= conn->in_cap - 1) {
        /* Buffer full, request still incomplete (in_off is 0 here: SERVE_NEED_MORE compacted it).
         * No header terminator yet means the headers themselves are too big (431, the Slowloris
         * shape). With headers complete it is only a body larger than the buffer, so grow (never
         * masks an oversized-header attack). */
        size_t header_len;
        int chunked;
        const int content_length = request_framing(conn->in_buf, conn->in_len, &header_len, &chunked, NULL, NULL);
        if (header_len == 0) {
            reject_request(app, conn, 431);
            return -1;
        }
        const int grown = grow_in_buf(app, conn, header_len, chunked, content_length);
        if (grown == 1) {
            return 0; /* wait for more read events */
        }
        reject_request(app, conn, grown == -1 ? 413 : 500);
        return -1;
    }
    return 0;
}

void handle_readable(App *app, Connection *conn) {
    if (read_and_serve(app, conn) == 0) {
        stop_borrowing_read_buf(app, conn); /* M2: App.read_buf is never held across event-loop turns */
    }
}

void handle_writable(App *app, Connection *conn) {
    if (response_pending(conn)) {
        if (flush_connection(app, conn) != FLUSH_DONE) {
            return; /* still draining, or closed */
        }
    } else {
        /* Woken only to resume a pipeline that hit MAX_PIPELINED_PER_EVENT (serve_buffered_requests). */
        event_loop_unwatch_write(app, conn->fd, conn);
    }
    if (conn->in_len > conn->in_off) {
        int served;
        serve_buffered_requests(app, conn, &served);
    }
}

void close_idle_connections(App *app) {
    const time_t now = time(NULL);

    for (int fd = 0; fd < app->connections_cap; fd++) {
        Connection *conn = app->connections[fd];
        if (conn == NULL) {
            continue;
        }

        /* A write still in flight is a slow-reader-on-the-response problem, not the
         * slow-sender-of-a-request problem the checks below target, so it is bounded separately here
         * (S2): a pending response that hasn't accepted a single byte onto the socket in
         * WRITE_TIMEOUT_SECONDS is a client that stopped reading, not a slow one - close it rather
         * than hold the fd, arena and out_buf forever. Still-progressing writes (however slowly) are
         * left for flush_connection()/EVFILT_WRITE to keep draining. */
        if (conn->out_buf != NULL || conn->file_fd >= 0) {
            if (now - conn->last_write_progress >= WRITE_TIMEOUT_SECONDS) {
                connection_close(app, conn); /* a response is already mid-flight: nothing left to say */
            }
            continue;
        }

        /* A request in flight is bounded by request_started, independent of how often a byte arrives -
         * closes the slow-drip case (one byte every N < 60s) that never goes "idle" under last_activity
         * alone (S1). Headers incomplete: header deadline; headers already complete (body still
         * pending): the more generous body deadline. request_framing is cheap and this only runs once
         * per sweep second per connection, not on the per-byte hot path. */
        if (conn->request_started != 0) {
            size_t header_len = 0;
            int chunked = 0;
            if (conn->in_len > conn->in_off) {
                /* P9: from in_off - bytes before it belong to a request already answered. */
                request_framing(conn->in_buf + conn->in_off, conn->in_len - conn->in_off, &header_len, &chunked,
                                NULL, NULL);
            }
            const int headers_complete = header_len > 0;
            const time_t deadline = headers_complete ? REQUEST_BODY_TIMEOUT_SECONDS : REQUEST_HEADER_TIMEOUT_SECONDS;
            if (now - conn->request_started < deadline) {
                continue;
            }
            if (conn->in_len > 0) {
                reject_request(app, conn, 408);
            } else {
                connection_close(app, conn); /* nothing received yet: no 408 to send */
            }
            continue;
        }

        if (now - conn->last_activity < IDLE_TIMEOUT_SECONDS) {
            continue;
        }

        if (conn->in_len > 0) {
            reject_request(app, conn, 408); /* went quiet mid-request: say why, then close */
        } else {
            connection_close(app, conn); /* idle keep-alive: nothing to answer */
        }
    }
}

void app_on_worker_start(App *app, WorkerInitHook hook) {
    if (app->worker_init_hook_count >= MAX_WORKER_INIT_HOOKS) {
        fprintf(stderr, "app_on_worker_start: MAX_WORKER_INIT_HOOKS (%d) exceeded, dropping hook\n",
                MAX_WORKER_INIT_HOOKS);
        return;
    }
    app->worker_init_hooks[app->worker_init_hook_count++] = hook;
}

static void run_worker_init_hooks(App *app) {
    for (int i = 0; i < app->worker_init_hook_count; i++) {
        app->worker_init_hooks[i]();
    }
}

void app_listen_worker(App *app, int port) {
    /* Ignore SIGPIPE so a write() to a socket the peer already closed
     * (a client disconnecting mid-response - routine under real load, not
     * an error condition) returns EPIPE instead of terminating this
     * process outright. Set here rather than once in main(), since this is
     * the one function every serving process (standalone, or each
     * individual forked cluster worker) always runs before touching a
     * socket - same choke point run_worker_init_hooks (above) relies on. */
    signal(SIGPIPE, SIG_IGN);

    run_worker_init_hooks(app);

    /* C4 (CEXPRESS_SINGLE_ACCEPTOR only): a worker running under the single-acceptor cluster model
     * arrives here with server_fd already set to its control socket by
     * app_listen_worker_via_control_socket, and must not clobber it with a real listen socket of its
     * own - it never accept()s directly. Every other caller (standalone, or a Linux cluster worker)
     * is unaffected: accept_via_fd_passing is 0 for them, same bind-here behavior as before. */
    if (!app->accept_via_fd_passing) {
        app->server_fd = create_server_socket(port);
        if (app->server_fd < 0) {
            fprintf(stderr, "app_listen_worker: could not create listening socket on port %d\n", port);
            exit(EXIT_FAILURE);
        }
    }
    if (event_loop_init(app) != 0) {
        perror("event_loop_init");
        exit(EXIT_FAILURE);
    }
    event_loop_watch_read(app, app->server_fd, NULL);

    if (!app->accept_via_fd_passing && (!cluster_is_worker() || cluster_worker_id() == 0)) {
        printf("Listening on port %d\n", port);
    }

    LoopEvent events[MAX_EVENTS];
    while (1) {
        int n = event_loop_poll(app, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EBADF && app->is_shutting_down) {
                break;
            }
            perror("event_loop_poll");
            break;
        }

        for (int i = 0; i < n; i++) {
            LoopEvent *ev = &events[i];

            if (ev->type == LOOP_EVENT_SIGNAL) {
                if (!app->is_shutting_down) {
                    printf("\nReceived signal %d, draining connections...\n", ev->signo);
                    app_stop(app);
                    if (app_count_connections(app) == 0) {
                        goto shutdown_complete;
                    }
                } else {
                    fprintf(stderr, "\nReceived second signal %d, forcing shutdown\n", ev->signo);
                    goto shutdown_complete;
                }
                continue;
            }

            if (ev->type == LOOP_EVENT_TIMER_IDLE) {
                close_idle_connections(app);
                continue;
            }

            if (ev->type == LOOP_EVENT_TIMER_SHUTDOWN) {
                fprintf(stderr, "Shutdown timeout reached (%ds), force-closing remaining connections\n",
                        SHUTDOWN_TIMEOUT_SECONDS);
                goto shutdown_complete;
            }

            if (ev->type == LOOP_EVENT_ACCEPT) {
#ifdef CEXPRESS_SINGLE_ACCEPTOR
                if (app->accept_via_fd_passing) {
                    accept_passed_connections(app);
                    continue;
                }
#endif
                accept_connections(app);
                continue;
            }

            int fd = ev->fd;
            if (fd < 0 || fd >= app->connections_cap || app->connections[fd] == NULL ||
                app->connections[fd] != ev->conn) {
                /* Connection was closed earlier in this event batch (e.g. by app_stop
                 * or close_idle_connections) - skip to avoid use-after-free. */
                continue;
            }

            Connection *conn = ev->conn;

            if (ev->type == LOOP_EVENT_ERROR) {
                connection_close(app, conn);
                continue;
            }

            if (ev->type == LOOP_EVENT_READ) {
                handle_readable(app, conn);
            } else if (ev->type == LOOP_EVENT_WRITE) {
                handle_writable(app, conn);
            }
        }

        if (app->is_shutting_down && app_count_connections(app) == 0) {
            printf("All connections drained. Server shutting down.\n");
            break;
        }
    }

shutdown_complete:
    if (app->connections != NULL) {
        for (int fd = 0; fd < app->connections_cap; fd++) {
            if (app->connections[fd] != NULL) {
                connection_close(app, app->connections[fd]);
            }
        }
    }
    event_loop_close(app);
}

void app_listen_worker_via_control_socket(App *app, int control_fd) {
    app->server_fd = control_fd;
    app->accept_via_fd_passing = 1;
    app_listen_worker(app, 0); /* port unused: server_fd is already the control socket, not bound here */
}

void app_listen(App *app, int port) {
    int workers = cluster_resolve_worker_count(app->config.workers);
    if (workers > 1 && !cluster_is_worker()) {
        cluster_listen(app, port, workers);
        return;
    }
    app_listen_worker(app, port);
}

void app_listen_cluster(App *app, int port, int num_workers) {
    cluster_listen(app, port, num_workers);
}

