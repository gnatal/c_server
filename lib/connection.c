#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/event.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "connection.h"
#include "httpParser.h"
#include "router.h"
#include "response.h"

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
    conn->fd = fd;
    return conn;
}

void connection_close(App *app, Connection *conn) {
    struct kevent changes[2];
    EV_SET(&changes[0], conn->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&changes[1], conn->fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    kevent(app->kq, changes, 2, NULL, 0, NULL);

    close(conn->fd);
    app->connections[conn->fd] = NULL;
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
    } else {
        connection_close(app, conn);
    }
}

void handle_readable(App *app, Connection *conn) {
    while (conn->in_len < BUF_SIZE - 1) {
        ssize_t n = recv(conn->fd, conn->in_buf + conn->in_len, BUF_SIZE - 1 - conn->in_len, 0);
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

        if (request_is_complete(conn->in_buf, conn->in_len)) {
            Request req;
            Response res;
            res.conn = conn;
            res.status = 200;

            if (parse_http_request(conn->in_buf, &req) != 0) {
                conn->keep_alive = 0;
                res_status(&res, 400);
                res_send(&res, "Bad Request");
            } else {
                conn->keep_alive = !request_wants_close(&req);
                const Route *route = match_route(app, &req);
                if (route != NULL) {
                    route->handler(&req, &res);
                } else {
                    res_status(&res, 404);
                    res_send(&res, "Not Found");
                }
            }

            flush_connection(app, conn);
            return;
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
