#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
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

void accept_connections(App *app) {
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(app->server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            break;
        }

        if (client_fd >= MAX_CONNECTIONS) {
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
                } else if (extract_content_length(conn->in_buf) == -2) {
                    res_status(&res, 413);
                    res_send(&res, "Payload Too Large");
                } else {
                    res_status(&res, 400);
                    res_send(&res, "Bad Request");
                }
            } else {
                conn->keep_alive = !request_wants_close(&req);
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
         *     and dispatched a 413 the moment headers arrived (see the
         *     extract_content_length == -2 check above). So content_length
         *     here is always valid and within MAX_BODY_SIZE - grow in_buf
         *     via realloc to exactly header+body+NUL and keep waiting for
         *     more EVFILT_READ events instead of rejecting - this is what
         *     lets a body exceed BUF_SIZE (see pending.txt, "no streaming",
         *     and lib/CLAUDE.md, "Body buffering"). Only a failed realloc
         *     (genuine server-side OOM, not a client-declared size problem)
         *     falls through to a rejection below, as 500.
         */
        const char *header_end = strstr(conn->in_buf, "\r\n\r\n");
        if (header_end != NULL) {
            const int content_length = extract_content_length(conn->in_buf);
            if (content_length >= 0) {
                const size_t header_len = (size_t)(header_end + 4 - conn->in_buf);
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

    for (int fd = 0; fd < MAX_CONNECTIONS; fd++) {
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
            perror("kevent");
            break;
        }

        for (int i = 0; i < n; i++) {
            struct kevent *ev = &events[i];

            if (ev->filter == EVFILT_TIMER) {
                close_idle_connections(app);
                continue;
            }

            if ((int)ev->ident == app->server_fd) {
                accept_connections(app);
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
    }
}
