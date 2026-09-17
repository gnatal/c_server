#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/event.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "connection.h"
#include "http_parser.h"
#include "router.h"
#include "response.h"
#include "middleware.h"

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

int create_server_socket(int port) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    const int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, BACKLOG) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    set_nonblocking(server_fd);

    return server_fd;
}

Connection *connection_create(int fd) {
    Connection *conn = calloc(1, sizeof(Connection));
    if (conn == NULL) {
        return NULL;
    }
    conn->in_buf = malloc(BUF_SIZE);
    if (conn->in_buf == NULL) {
        free(conn);
        return NULL;
    }
    conn->in_cap = BUF_SIZE;
    conn->fd = fd;
    conn->last_activity = time(NULL);
    return conn;
}

void connection_close(App *app, Connection *conn) {
    struct kevent changes[2];
    EV_SET(&changes[0], conn->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&changes[1], conn->fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    kevent(app->kq, changes, 2, NULL, 0, NULL);

    close(conn->fd);
    app->connections[conn->fd] = NULL;
    free(conn->in_buf);
    free(conn->out_buf);
    free(conn);
}

void app_destroy(App *app) {
    for (int fd = 0; fd < app->connections_cap; fd++) {
        if (app->connections[fd] != NULL) {
            connection_close(app, app->connections[fd]);
        }
    }
    if (app->kq >= 0) {
        close(app->kq);
        app->kq = -1;
    }
    if (app->server_fd >= 0) {
        close(app->server_fd);
        app->server_fd = -1;
    }
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

    /* Stop accepting new connections: deregister from kqueue and close server socket */
    if (app->server_fd >= 0) {
        if (app->kq >= 0) {
            kq_unwatch(app->kq, app->server_fd, EVFILT_READ);
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
            if (conn->in_len == 0 && conn->out_buf == NULL) {
                connection_close(app, conn);
            } else {
                conn->keep_alive = 0;
            }
        }
    }

    /* Arm a oneshot shutdown deadline timer on kqueue so slow/stalled clients
     * cannot prevent the server process from exiting indefinitely. */
    if (app->kq >= 0) {
        struct kevent shutdown_timer;
        EV_SET(&shutdown_timer, 2, EVFILT_TIMER, EV_ADD | EV_ENABLE | EV_ONESHOT, 0,
               SHUTDOWN_TIMEOUT_SECONDS * 1000, NULL);
        kevent(app->kq, &shutdown_timer, 1, NULL, 0, NULL);
    }
}

void kq_watch(int kq, int fd, int16_t filter, void *udata) {
    struct kevent change;
    EV_SET(&change, fd, filter, EV_ADD | EV_ENABLE, 0, 0, udata);
    kevent(kq, &change, 1, NULL, 0, NULL);
}

void kq_unwatch(int kq, int fd, int16_t filter) {
    struct kevent change;
    EV_SET(&change, fd, filter, EV_DELETE, 0, 0, NULL);
    kevent(kq, &change, 1, NULL, 0, NULL);
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

void accept_connections(App *app) {
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(app->server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            break;
        }

        /* fd is bounded only by the process's own RLIMIT_NOFILE (accept()
         * itself starts failing with EMFILE once that's hit) - no fixed
         * ceiling here, just grow the table to fit. A failed grow (genuine
         * OOM) rejects just this one connection, without affecting any
         * already-open ones. */
        if (ensure_connection_capacity(app, client_fd) != 0) {
            close(client_fd);
            continue;
        }

        set_nonblocking(client_fd);
        const int nodelay = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        Connection *conn = connection_create(client_fd);
        if (conn == NULL) {
            close(client_fd);
            continue;
        }
        app->connections[client_fd] = conn;
        kq_watch(app->kq, client_fd, EVFILT_READ, conn);
    }
}

void flush_connection(App *app, Connection *conn) {
    while (conn->out_sent < conn->out_len) {
        ssize_t n = write(conn->fd, conn->out_buf + conn->out_sent, conn->out_len - conn->out_sent);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                kq_watch(app->kq, conn->fd, EVFILT_WRITE, conn);
                return;
            }
            connection_close(app, conn);
            return;
        }
        conn->out_sent += (size_t)n;
    }

    if (conn->keep_alive) {
        /* Drop any EVFILT_WRITE registration from a partial write above -
         * left registered with nothing queued, kqueue would report this fd
         * write-ready on every single loop iteration forever. */
        kq_unwatch(app->kq, conn->fd, EVFILT_WRITE);
        free(conn->out_buf);
        conn->out_buf = NULL;
        conn->out_len = 0;
        conn->out_sent = 0;
        conn->in_len = 0;

        /* If handle_readable grew in_buf to fit a large body (in_cap >
         * BUF_SIZE), shrink it back down now that the connection is idle -
         * otherwise one big request would permanently inflate this
         * connection's memory footprint for as long as it stays open. A
         * failed shrink isn't fatal (realloc leaves the original block
         * untouched on failure) - just keep using the larger buffer. */
        if (conn->in_cap > BUF_SIZE) {
            char *shrunk = realloc(conn->in_buf, BUF_SIZE);
            if (shrunk != NULL) {
                conn->in_buf = shrunk;
                conn->in_cap = BUF_SIZE;
            }
        }
    } else {
        connection_close(app, conn);
    }
}

void handle_readable(App *app, Connection *conn) {
    while (conn->in_len < conn->in_cap - 1) {
        ssize_t n = recv(conn->fd, conn->in_buf + conn->in_len, conn->in_cap - 1 - conn->in_len, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            connection_close(app, conn);
            return;
        }
        if (n == 0) {
            connection_close(app, conn);
            return;
        }

        conn->in_len += (size_t)n;
        conn->in_buf[conn->in_len] = '\0';
        conn->last_activity = time(NULL);

        if (request_is_complete(conn->in_buf, conn->in_len)) {
            Request req;
            Response res;
            res.conn = conn;
            res.status = 200;
            res.header_count = 0;
            res.set_cookie_count = 0;
            res.is_head_request = 0;

            const int parse_status = parse_http_request(conn->in_buf, conn->in_len, &req);
            if (parse_status != 0) {
                conn->keep_alive = 0;
                /* parse_http_request distinguishes -2 (request-line path too
                 * long -> 414) from every other failure (-1). A -1 still
                 * folds together a Content-Length that's merely too large
                 * (rather than malformed/negative), which deserves 413, not
                 * 400 - re-check the same pure function connection.c already
                 * relies on elsewhere for status-code decisions (cheap: a
                 * strcasestr + atol scan over headers already sitting in
                 * conn->in_buf). */
                if (parse_status == -2) {
                    res_status(&res, 414);
                    res_send(&res, "URI Too Long");
                } else if (req.content_length == -2) {
                    /* parse_http_request sets this same sentinel whether the
                     * rejection came from a Content-Length beyond
                     * MAX_BODY_SIZE or (see lib/CLAUDE.md, "Chunked
                     * Transfer-Encoding") a chunked body whose decoded size
                     * exceeds it - one check covers both, connection.c
                     * doesn't need to know which framing was used. */
                    res_status(&res, 413);
                    res_send(&res, "Payload Too Large");
                } else {
                    res_status(&res, 400);
                    res_send(&res, "Bad Request");
                }
            } else {
                conn->keep_alive = !request_wants_close(&req) && !app->is_shutting_down;
                /* HTTP forbids a body in any response to HEAD, regardless of
                 * status (see response.c: send_with_content_type). Set ahead
                 * of dispatch() so it applies uniformly whether the request
                 * resolves to a route's handler or to a 404/405 built by
                 * chain_next's fallback. */
                res.is_head_request = strcmp(req.method, "HEAD") == 0;
                const Route *route = match_route(app, &req);
                dispatch(app, route, &req, &res);
            }
            free(req.body);

            flush_connection(app, conn);
            return;
        }
    }

    if (conn->in_len >= conn->in_cap - 1) {
        /*
         * The buffer is full and still doesn't hold a complete request.
         * Before rejecting outright, tell apart two very different cases:
         *   - The header block itself hasn't finished arriving yet (no
         *     "\r\n\r\n" seen) - this is the original oversized-header/
         *     Slowloris shape, unaffected by body growth below, since
         *     growth only ever happens once headers are already complete.
         *     Reject with 431, same as always.
         *   - Headers ARE complete and this is purely a body that doesn't
         *     fit in the current capacity yet. A Content-Length beyond
         *     MAX_BODY_SIZE can never reach this branch in the first place -
         *     request_is_complete (called on every recv() above, including
         *     the one that completed the header block) already caught that
         *     and dispatched a 413 the moment headers arrived (see
         *     req.content_length == -2 above). So content_length here is
         *     always valid and within MAX_BODY_SIZE - grow in_buf via
         *     realloc to exactly header+body+NUL and keep waiting for more
         *     EVFILT_READ events instead of rejecting - this is what lets a
         *     body exceed BUF_SIZE (see lib/CLAUDE.md, "Body buffering").
         *     Only a failed realloc (genuine server-side OOM, not a
         *     client-declared size problem) falls through to a rejection
         *     below, as 500. A Transfer-Encoding: chunked body takes a
         *     separate branch just below instead, since it has no single
         *     declared size to realloc straight to (see lib/CLAUDE.md,
         *     "Chunked Transfer-Encoding").
         */
        const char *header_end = strstr(conn->in_buf, "\r\n\r\n");
        if (header_end != NULL) {
            const size_t header_len = (size_t)(header_end + 4 - conn->in_buf);

            if (request_has_chunked_encoding(conn->in_buf)) {
                /*
                 * A chunked body's total size isn't known upfront the way a
                 * declared Content-Length is (there's no single number to
                 * realloc straight to) - grow geometrically instead, capped
                 * at header_len + MAX_BODY_SIZE: the same ceiling a
                 * Content-Length body gets, applied here to the *raw* wire
                 * size rather than the decoded size chunked_body_scan/
                 * request_is_complete already bounds independently. Without
                 * this raw-side cap too, a client could inflate memory use
                 * well past MAX_BODY_SIZE by sending the same decoded byte
                 * count as a pile of pathologically tiny chunks (each
                 * "1\r\nX\r\n" chunk costs 6 raw bytes per 1 decoded byte)
                 * before chunked_body_scan's decoded-size check ever
                 * triggers - see lib/CLAUDE.md, "Chunked Transfer-Encoding".
                 * By the time this branch runs, request_is_complete already
                 * requires chunked_body_scan to have returned exactly 0 on
                 * the current buffer (1/-1/-2 all end the request in the
                 * dispatch branch above instead) - reaching here always
                 * means "still incomplete", never "reject this specific
                 * scan result".
                 */
                const size_t raw_cap = header_len + MAX_BODY_SIZE;
                if (conn->in_cap < raw_cap) {
                    size_t needed = conn->in_cap * 2;
                    if (needed > raw_cap) {
                        needed = raw_cap;
                    }
                    char *grown = realloc(conn->in_buf, needed);
                    if (grown != NULL) {
                        conn->in_buf = grown;
                        conn->in_cap = needed;
                        return; /* wait for more EVFILT_READ events */
                    }
                }

                Response res;
                res.conn = conn;
                res.header_count = 0;
                res.set_cookie_count = 0;
                res.is_head_request = 0;
                conn->keep_alive = 0;
                res_status(&res, 413);
                res_send(&res, "Payload Too Large");
                flush_connection(app, conn);
                return;
            }

            const int content_length = extract_content_length(conn->in_buf);
            if (content_length >= 0) {
                const size_t needed = header_len + (size_t)content_length + 1;
                char *grown = realloc(conn->in_buf, needed);
                if (grown != NULL) {
                    conn->in_buf = grown;
                    conn->in_cap = needed;
                    return; /* wait for more EVFILT_READ events */
                }
            }
        }

        Response res;
        res.conn = conn;
        res.header_count = 0;
        res.set_cookie_count = 0;
        res.is_head_request = 0;
        conn->keep_alive = 0;
        if (header_end == NULL) {
            res_status(&res, 431);
            res_send(&res, "Request Header Fields Too Large");
        } else {
            res_status(&res, 500);
            res_send(&res, "Internal Server Error");
        }
        flush_connection(app, conn);
    }
}

void close_idle_connections(App *app) {
    const time_t now = time(NULL);

    for (int fd = 0; fd < app->connections_cap; fd++) {
        Connection *conn = app->connections[fd];
        if (conn == NULL) {
            continue;
        }

        /* A write still in flight is a slow-reader-on-the-response problem,
         * not the slow-sender-of-a-request problem this timeout targets -
         * leave it for flush_connection()/EVFILT_WRITE to keep draining. */
        if (conn->out_buf != NULL) {
            continue;
        }

        if (now - conn->last_activity < IDLE_TIMEOUT_SECONDS) {
            continue;
        }

        if (conn->in_len > 0) {
            /* A request was in progress when the client went quiet - let it
             * know why before closing, same shape as the 431/500 rejections
             * in handle_readable(). */
            Response res;
            res.conn = conn;
            res.header_count = 0;
            res.set_cookie_count = 0;
            res.is_head_request = 0;
            conn->keep_alive = 0;
            res_status(&res, 408);
            res_send(&res, "Request Timeout");
            flush_connection(app, conn);
        } else {
            /* Idle keep-alive connection that never sent a next request -
             * nothing to respond to. */
            connection_close(app, conn);
        }
    }
}

void app_listen(App *app, int port) {
    app->server_fd = create_server_socket(port);
    app->kq = kqueue();
    if (app->kq < 0) {
        perror("kqueue");
        exit(EXIT_FAILURE);
    }
    kq_watch(app->kq, app->server_fd, EVFILT_READ, NULL);

    /*
     * Intercept SIGINT and SIGTERM for graceful shutdown. Ignore their default
     * actions (which would terminate the process asynchronously) and monitor
     * them synchronously via kqueue's EVFILT_SIGNAL.
     */
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);

    struct kevent sig_changes[2];
    EV_SET(&sig_changes[0], SIGINT, EVFILT_SIGNAL, EV_ADD | EV_ENABLE, 0, 0, NULL);
    EV_SET(&sig_changes[1], SIGTERM, EVFILT_SIGNAL, EV_ADD | EV_ENABLE, 0, 0, NULL);
    kevent(app->kq, sig_changes, 2, NULL, 0, NULL);

    /*
     * Periodic timer that drives close_idle_connections() - ident 1 is
     * arbitrary and never collides with a real connection fd: EVFILT_TIMER
     * idents live in their own namespace, separate from the fd-based idents
     * EVFILT_READ/EVFILT_WRITE use below.
     */
    struct kevent timer_change;
    EV_SET(&timer_change, 1, EVFILT_TIMER, EV_ADD | EV_ENABLE, 0, IDLE_SWEEP_INTERVAL_MS, NULL);
    kevent(app->kq, &timer_change, 1, NULL, 0, NULL);

    printf("Listening on port %d\n", port);

    struct kevent events[MAX_EVENTS];
    while (1) {
        int n = kevent(app->kq, NULL, 0, events, MAX_EVENTS, NULL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EBADF && app->is_shutting_down) {
                break;
            }
            perror("kevent");
            break;
        }

        for (int i = 0; i < n; i++) {
            struct kevent *ev = &events[i];

            if (ev->filter == EVFILT_SIGNAL) {
                if (!app->is_shutting_down) {
                    printf("\nReceived signal %ld, draining connections...\n", (long)ev->ident);
                    app_stop(app);
                    if (app_count_connections(app) == 0) {
                        goto shutdown_complete;
                    }
                } else {
                    fprintf(stderr, "\nReceived second signal %ld, forcing shutdown\n", (long)ev->ident);
                    goto shutdown_complete;
                }
                continue;
            }

            if (ev->filter == EVFILT_TIMER) {
                if (ev->ident == 1) {
                    close_idle_connections(app);
                } else if (ev->ident == 2) {
                    fprintf(stderr, "Shutdown timeout reached (%ds), force-closing remaining connections\n",
                            SHUTDOWN_TIMEOUT_SECONDS);
                    goto shutdown_complete;
                }
                continue;
            }

            if (app->server_fd >= 0 && (int)ev->ident == app->server_fd) {
                accept_connections(app);
                continue;
            }

            int fd = (int)ev->ident;
            if (fd < 0 || fd >= app->connections_cap || app->connections[fd] == NULL ||
                app->connections[fd] != (Connection *)ev->udata) {
                /* Connection was closed earlier in this event batch (e.g. by app_stop
                 * or close_idle_connections) - skip to avoid use-after-free. */
                continue;
            }

            Connection *conn = (Connection *)ev->udata;

            if (ev->flags & EV_ERROR) {
                connection_close(app, conn);
                continue;
            }

            /*
             * Note: EV_EOF can be set on a readable event even while there is
             * still unread data (the peer half-closed after sending). Don't
             * close here - let handle_readable()/flush_connection() drain
             * what's available and close naturally once recv()/write() see
             * the actual end of stream or an error.
             */
            if (ev->filter == EVFILT_READ) {
                handle_readable(app, conn);
            } else if (ev->filter == EVFILT_WRITE) {
                flush_connection(app, conn);
            }
        }

        if (app->is_shutting_down && app_count_connections(app) == 0) {
            printf("All connections drained. Server shutting down.\n");
            break;
        }
    }

shutdown_complete:
    /* Restore default signal dispositions */
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
}
