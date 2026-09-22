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
#include "tls.h"

static ssize_t conn_read(App *app, Connection *conn, void *buf, size_t count) {
    if (conn->ssl != NULL) {
        return tls_connection_read(app, conn, buf, count);
    }
    return recv(conn->fd, buf, count, 0);
}

static ssize_t conn_write(App *app, Connection *conn, const void *buf, size_t count) {
    if (conn->ssl != NULL) {
        return tls_connection_write(app, conn, buf, count);
    }
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

int create_server_socket(int port) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    const int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR");
        exit(EXIT_FAILURE);
    }

#ifdef SO_REUSEPORT
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEPORT");
    }
#endif

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    /* S3: take whichever is larger - BACKLOG is a floor, not a cap. No-op on a system whose
     * SOMAXCONN is already <= BACKLOG (e.g. macOS, where kern.ipc.somaxconn is 128, same as
     * BACKLOG); on a Linux whose SOMAXCONN is raised, this actually widens the pending-accept
     * queue instead of the kernel silently dropping SYNs past 128. */
    const int backlog = BACKLOG > SOMAXCONN ? BACKLOG : SOMAXCONN;
    if (listen(server_fd, backlog) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    set_nonblocking(server_fd);

    return server_fd;
}

#define ARENA_SIZE (64 * 1024)

Connection *connection_create(int fd) {
    Connection *conn = calloc(1, sizeof(Connection) + ARENA_SIZE);
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
    conn->file_fd = -1;
    conn->last_activity = time(NULL);
    /* request_started stays 0 (calloc) until a request actually starts arriving, or (tls_connection_init)
     * a TLS handshake begins: a freshly accepted, otherwise-silent connection is bounded by
     * IDLE_TIMEOUT_SECONDS below, same as before (S1 targets a request/handshake that is under way but
     * moving too slowly, not one that never starts). */
    arena_init(&conn->arena, (char *)(conn + 1), ARENA_SIZE);
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

    tls_connection_close(app, conn);

    if (conn->fd >= 0) {
        close(conn->fd);
        conn->fd = -1;
    }
    free(conn->in_buf);
    conn->in_buf = NULL;
    conn->out_buf = NULL;
    arena_destroy(&conn->arena);
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
    tls_cleanup_app(app);
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
 * retry the write, defeating the purpose of shedding cheaply). For a TLS listener the client
 * expects a TLS handshake, not plaintext HTTP, so writing this would just look like protocol
 * garbage instead of a 503 - skip straight to closing there.
 */
static void reject_overloaded_connection(int client_fd, int is_tls) {
    if (!is_tls) {
        const char *body = status_text(503);
        char head[160];
        int head_len = snprintf(head, sizeof(head),
                                 "HTTP/1.1 503 %s\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n"
                                 "Connection: close\r\n\r\n",
                                 body, strlen(body));
        set_nonblocking(client_fd);
        if (head_len > 0 && (size_t)head_len < sizeof(head)) {
            ssize_t n = write(client_fd, head, (size_t)head_len);
            if (n == head_len) {
                write(client_fd, body, strlen(body));
            }
        }
    }
    close(client_fd);
}

void accept_connections(App *app) {
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(app->server_fd, (struct sockaddr *)&client_addr, &client_len);
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

        /* S3: shed load past the configured cap instead of accumulating connections (and their
         * arenas) without bound. Checked before ensure_connection_capacity/connection_create so an
         * overloaded server doesn't pay for either. open_connections is O(1) (see App.open_connections)
         * - this runs once per accepted connection, including every one we're about to reject, so it
         * must not become the O(n) scan it's replacing. */
        if (app->config.max_connections > 0 && app->open_connections >= app->config.max_connections) {
            reject_overloaded_connection(client_fd, app->ssl_ctx != NULL);
            continue;
        }

        /* Past the cap above, fd is bounded only by the process's own RLIMIT_NOFILE (accept()
         * itself starts failing with EMFILE once that's hit, handled above) - no fixed ceiling
         * here, just grow the table to fit. A failed grow (genuine OOM) rejects just this one
         * connection, without affecting any already-open ones. */
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
        app->open_connections++; /* paired with connection_close's -- (S3) */

        if (app->ssl_ctx != NULL) {
            if (tls_connection_init(app, conn) != 0) {
                connection_close(app, conn);
                continue;
            }
            int hs = tls_connection_handshake(app, conn);
            if (hs < 0) {
                connection_close(app, conn);
                continue;
            }
        } else {
            event_loop_watch_read(app, client_fd, conn);
        }
    }
}

void flush_connection(App *app, Connection *conn) {
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
            ssize_t n = conn_write(app, conn, conn->out_buf + conn->out_sent, conn->out_len - conn->out_sent);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    event_loop_watch_write(app, conn->fd, conn);
                    return;
                }
                connection_close(app, conn);
                return;
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
                    event_loop_watch_write(app, conn->fd, conn);
                    return;
                }

                size_t to_read = conn->file_remaining < STREAM_CHUNK_SIZE
                                     ? conn->file_remaining
                                     : STREAM_CHUNK_SIZE;
                if (conn->out_cap < to_read || conn->out_buf == NULL) {
                    conn->out_buf = arena_alloc(&conn->arena, to_read);
                    if (conn->out_buf == NULL) {
                        connection_close(app, conn);
                        return;
                    }
                    conn->out_cap = to_read;
                }
                ssize_t r = read(conn->file_fd, conn->out_buf, to_read);
                if (r <= 0) {
                    connection_close(app, conn);
                    return;
                }
                conn->out_len = (size_t)r;
                conn->out_sent = 0;
                conn->out_cap = to_read;
                conn->file_remaining -= (size_t)r;
                continue;
            } else {
                close(conn->file_fd);
                conn->file_fd = -1;
            }
        }

        break;
    }

    if (conn->keep_alive) {
        /* Drop write registration from a partial write above */
        event_loop_unwatch_write(app, conn->fd, conn);
        conn->out_buf = NULL;
        conn->out_len = 0;
        conn->out_sent = 0;
        conn->out_cap = 0;
        conn->in_len = 0;
        conn->request_started = 0; /* back to idle between requests: only IDLE_TIMEOUT_SECONDS applies (S1) */
        conn->last_write_progress = 0; /* no response pending: WRITE_TIMEOUT_SECONDS stops applying (S2) */
        arena_reset(&conn->arena);

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

        if (tls_has_pending(conn)) {
            handle_readable(app, conn);
            return;
        }
    } else {
        connection_close(app, conn);
    }
}

/* Sends `status` with its reason phrase as the body, then closes the connection. */
static void reject_request(App *app, Connection *conn, const int status) {
    Response res;
    res_init(&res, conn);
    conn->keep_alive = 0;
    res_status(&res, status);
    res_send(&res, status_text(status));
    flush_connection(app, conn);
}

/*
 * in_buf is full with headers complete: grow it to fit the body. Returns 1 grown (keep reading),
 * 0 realloc failed (-> 500), -1 chunked raw-size cap hit (-> 413).
 * Content-Length: one realloc straight to header_len + content_length + 1 (size is known).
 * Chunked: doubling, capped at header_len + MAX_BODY_SIZE on the RAW wire size, so a client
 * cannot inflate memory past that by sending tiny chunks ("1\r\nX\r\n" = 6 raw bytes per byte).
 * A Content-Length above MAX_BODY_SIZE never gets here: request_is_complete already stopped
 * buffering and parse_http_request answered 413.
 */
static int grow_in_buf(Connection *conn, const size_t header_len, const int chunked, const int content_length) {
    size_t needed;
    if (chunked) {
        const size_t raw_cap = header_len + MAX_BODY_SIZE;
        if (conn->in_cap >= raw_cap) {
            return -1;
        }
        needed = conn->in_cap * 2 > raw_cap ? raw_cap : conn->in_cap * 2;
    } else if (content_length >= 0) {
        needed = header_len + (size_t)content_length + 1;
    } else {
        return 0;
    }
    char *grown = realloc(conn->in_buf, needed);
    if (grown == NULL) {
        return 0;
    }
    conn->in_buf = grown;
    conn->in_cap = needed;
    return 1;
}

void handle_readable(App *app, Connection *conn) {
    if (conn->request_started == 0) {
        /* First byte of a fresh request after an idle keep-alive gap: (re)start the deadline clock (S1). */
        conn->request_started = time(NULL);
    }

    while (conn->in_len < conn->in_cap - 1) {
        ssize_t n = conn_read(app, conn, conn->in_buf + conn->in_len, conn->in_cap - 1 - conn->in_len);
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
            const int parse_status = parse_http_request(conn->in_buf, conn->in_len, &req, &conn->arena);
            if (parse_status != 0) {
                /* -2: path too long (414). content_length == -2: body over MAX_BODY_SIZE (413),
                 * for both Content-Length and chunked framing. Anything else: 400. */
                /* req.body is managed by arena, no need to free */
                reject_request(app, conn, parse_status == -2 ? 414 : (req.content_length == -2 ? 413 : 400));
                return;
            }

            Response res;
            res_init(&res, conn);
            conn->keep_alive = !request_wants_close(&req) && !app->is_shutting_down;
            /* Set before dispatch so HEAD bodies are suppressed for matched routes and 404/405 alike. */
            res.is_head_request = strcmp(req.method, "HEAD") == 0;
            const Route *route = match_route(app, &req);
            dispatch(app, route, &req, &res);
            /* req.body is managed by arena, no need to free */

            flush_connection(app, conn);
            return;
        }
    }

    if (conn->in_len >= conn->in_cap - 1) {
        /* Buffer full, request still incomplete. No header terminator yet means the headers
         * themselves are too big (431, the Slowloris shape). With headers complete it is only
         * a body larger than the buffer, so grow (never masks an oversized-header attack). */
        size_t header_len;
        int chunked;
        const int content_length = request_framing(conn->in_buf, conn->in_len, &header_len, &chunked);
        if (header_len == 0) {
            reject_request(app, conn, 431);
            return;
        }
        const int grown = grow_in_buf(conn, header_len, chunked, content_length);
        if (grown == 1) {
            return; /* wait for more read events */
        }
        reject_request(app, conn, grown == -1 ? 413 : 500);
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

        /* A request (or TLS handshake) in flight is bounded by request_started, independent of how
         * often a byte arrives - closes the slow-drip case (one byte every N < 60s) that never goes
         * "idle" under last_activity alone (S1). Headers incomplete: header deadline; headers already
         * complete (body still pending): the more generous body deadline. request_framing is cheap and
         * this only runs once per sweep second per connection, not on the per-byte hot path. */
        if (conn->request_started != 0) {
            size_t header_len = 0;
            int chunked = 0;
            if (conn->in_len > 0) {
                request_framing(conn->in_buf, conn->in_len, &header_len, &chunked);
            }
            const int headers_complete = header_len > 0;
            const time_t deadline = headers_complete ? REQUEST_BODY_TIMEOUT_SECONDS : REQUEST_HEADER_TIMEOUT_SECONDS;
            if (now - conn->request_started < deadline) {
                continue;
            }
            if (conn->in_len > 0) {
                reject_request(app, conn, 408);
            } else {
                connection_close(app, conn); /* nothing received yet (e.g. stalled TLS handshake): no 408 to send */
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
     * process outright. Unconditional (not just under TLS - see
     * lib/tls.c's history) and set here rather than once in main(), since
     * this is the one function every serving process (standalone, or each
     * individual forked cluster worker) always runs before touching a
     * socket - same choke point run_worker_init_hooks (above) relies on. */
    signal(SIGPIPE, SIG_IGN);

    run_worker_init_hooks(app);

    if (app->config.tls_enabled && app->ssl_ctx == NULL) {
        if (tls_init_app(app) != 0) {
            fprintf(stderr, "Failed to initialize TLS with cert '%s' and key '%s'\n",
                    app->config.tls_cert_file, app->config.tls_key_file);
            exit(EXIT_FAILURE);
        }
    }

    app->server_fd = create_server_socket(port);
    if (event_loop_init(app) != 0) {
        perror("event_loop_init");
        exit(EXIT_FAILURE);
    }
    event_loop_watch_read(app, app->server_fd, NULL);

    if (!cluster_is_worker() || cluster_worker_id() == 0) {
        if (app->config.tls_enabled) {
            printf("Listening on port %d (HTTPS / TLS)\n", port);
        } else {
            printf("Listening on port %d\n", port);
        }
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

            if (conn->tls_state == TLS_STATE_HANDSHAKE) {
                int hs = tls_connection_handshake(app, conn);
                if (hs < 0) {
                    connection_close(app, conn);
                } else if (hs == 1) {
                    if (tls_has_pending(conn)) {
                        handle_readable(app, conn);
                    }
                }
                continue;
            }

            if (ev->type == LOOP_EVENT_READ) {
                handle_readable(app, conn);
            } else if (ev->type == LOOP_EVENT_WRITE) {
                flush_connection(app, conn);
            }
        }

        if (app->ssl_ctx != NULL) {
            for (int fd = 0; fd < app->connections_cap; fd++) {
                Connection *c = app->connections[fd];
                if (c != NULL && c->tls_state == TLS_STATE_CONNECTED &&
                    c->out_buf == NULL && tls_has_pending(c)) {
                    handle_readable(app, c);
                }
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

