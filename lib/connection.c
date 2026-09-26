#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/uio.h>
#if defined(__linux__)
#include <sys/sendfile.h>
#endif
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>
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

static ssize_t conn_writev(Connection *conn, const struct iovec *iov, int iovcnt) {
    return writev(conn->fd, iov, iovcnt);
}

/*
 * Sends up to `count` bytes of conn->file_fd from conn->file_offset straight from the page cache
 * (sendfile(2): no read into user memory). macOS also carries head[0..head_len) in front of the file
 * bytes in the same call (hdtr); on Linux head_len must be 0 (the head goes out through conn_write
 * first). *sent = bytes the socket accepted, head bytes first. Returns 0, or -1 with errno set - macOS
 * can report EAGAIN with *sent > 0 (a partial send). The file position is never used or moved.
 * Platforms without sendfile fail with ENOSYS, which flush_connection treats as "fall back to pread".
 */
#if defined(__APPLE__)
#define SENDFILE_CARRIES_HEAD 1
#else
#define SENDFILE_CARRIES_HEAD 0
#endif

static int conn_sendfile(Connection *conn, const char *head, const size_t head_len, const size_t count,
                         size_t *sent) {
#if defined(__APPLE__)
    struct iovec head_iov = { (void *)head, head_len };
    struct sf_hdtr hdtr = { &head_iov, 1, NULL, 0 };
    /* in: head + file bytes wanted (macOS counts hdtr bytes in len both ways; count is never 0 here,
     * 0 would mean "to EOF"); out: all bytes sent, head first */
    off_t len = (off_t)(head_len + count);
    const int rc = sendfile(conn->file_fd, conn->fd, conn->file_offset, &len, head_len > 0 ? &hdtr : NULL, 0);
    *sent = len > 0 ? (size_t)len : 0;
    return rc == 0 ? 0 : -1;
#elif defined(__linux__)
    (void)head;
    (void)head_len;
    off_t off = conn->file_offset;
    const ssize_t n = sendfile(conn->fd, conn->file_fd, &off, count);
    *sent = n > 0 ? (size_t)n : 0;
    return n < 0 ? -1 : 0;
#else
    (void)conn;
    (void)head;
    (void)head_len;
    (void)count;
    *sent = 0;
    errno = ENOSYS;
    return -1;
#endif
}

/* errno values meaning "sendfile cannot serve this fd pair", not "the connection failed". */
static int sendfile_unsupported(const int err) {
    return err == ENOSYS || err == EINVAL || err == ENOTSOCK || err == EOPNOTSUPP
#if defined(ENOTSUP) && ENOTSUP != EOPNOTSUPP
           || err == ENOTSUP
#endif
        ;
}

/* 1 while the file body goes out through sendfile (not the pread fallback) and, on macOS, the unsent head
 * rides in front of it in the same call instead of its own write. */
static int head_rides_sendfile(const Connection *conn) {
    return SENDFILE_CARRIES_HEAD && conn->file_fd >= 0 && conn->file_remaining > 0 && !conn->file_no_sendfile;
}

/* Bytes of conn's shared_body not yet written (0 when none is pinned). */
static size_t shared_body_left(const Connection *conn) {
    return conn->shared_body != NULL ? conn->shared_body->len - conn->shared_body_sent : 0;
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

/* returns -1 on any failure (socket already closed first) instead of perror()+exit() - a
 * library function shouldn't unilaterally kill the process it's linked into. The two library-side
 * callers (app_listen_worker below, cluster_listen in cluster.c) are the ones that decide a failure
 * here is fatal and exit() themselves, same convention event_loop_init already uses (see lib/CLAUDE.md,
 * "Workers and fork"/"Return conventions") - the decision just now lives at that outer boundary
 * instead of being forced deep inside socket setup. */
/* 1 when s is a canonical numeric address: dotted-quad IPv4, or IPv6 with an optional "%scope" suffix
 * (link-local). getaddrinfo's AI_NUMERICHOST alone would also take inet_aton's legacy shorthands
 * ("1.2.3", "127.1", "0x7f.1"), which name addresses nobody reading the config would expect. */
static int is_numeric_address(const char *s) {
    unsigned char addr[sizeof(struct in6_addr)];
    if (inet_pton(AF_INET, s, addr) == 1) return 1;
    char v6[INET6_ADDRSTRLEN];
    const size_t n = strcspn(s, "%");
    if (n >= sizeof(v6)) return 0;
    memcpy(v6, s, n);
    v6[n] = '\0';
    return inet_pton(AF_INET6, v6, addr) == 1 && (s[n] == '\0' || s[n + 1] != '\0');
}

int create_server_socket(const char *bind_address, const int port) {
    /* NULL keeps the historical listener, every IPv4 interface. A literal address only
     * (AI_NUMERICHOST): no DNS lookup at startup, and no name like "localhost" that resolves to
     * several addresses of which only the first would be bound. */
    const char *const node = bind_address != NULL ? bind_address : "0.0.0.0";
    if (!is_numeric_address(node)) {
        fprintf(stderr, "create_server_socket: bind address \"%s\" is not a numeric IPv4/IPv6 address\n", node);
        return -1;
    }
    char service[8];
    snprintf(service, sizeof(service), "%d", port);
    const struct addrinfo hints = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
        .ai_flags = AI_PASSIVE | AI_NUMERICHOST | AI_NUMERICSERV,
    };
    struct addrinfo *addr = NULL; /* freed by freeaddrinfo below, on every path past this call */
    const int gai = getaddrinfo(node, service, &hints, &addr);
    if (gai != 0) {
        fprintf(stderr, "create_server_socket: bind address \"%s\" is not a numeric IPv4/IPv6 address: %s\n",
                node, gai_strerror(gai));
        return -1;
    }

    int server_fd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
    if (server_fd < 0) {
        perror("socket");
        freeaddrinfo(addr);
        return -1;
    }

    const int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR");
        close(server_fd);
        freeaddrinfo(addr);
        return -1;
    }

    if (addr->ai_family == AF_INET6) {
        /* "::" means every interface, IPv4 (as mapped addresses) included, on every platform:
         * Linux defaults IPV6_V6ONLY to 0, the BSDs and macOS to 1. */
        const int v6only = 0;
        if (setsockopt(server_fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) < 0) {
            perror("setsockopt IPV6_V6ONLY");
            close(server_fd);
            freeaddrinfo(addr);
            return -1;
        }
    }

#ifdef SO_REUSEPORT
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEPORT");
    }
#endif

    /* set once here instead of once per accepted connection - accepted sockets inherit it from
     * the listener (MEASURED on macOS 26 and Linux 6.8; asserted by test_connection.c's
     * test_accept_client_socket_options so a platform that stops inheriting it fails a test instead
     * of silently turning Nagle back on). */
    if (setsockopt(server_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0) {
        perror("setsockopt TCP_NODELAY");
        close(server_fd);
        freeaddrinfo(addr);
        return -1;
    }

    const int bound = bind(server_fd, addr->ai_addr, addr->ai_addrlen);
    freeaddrinfo(addr);
    if (bound < 0) {
        perror("bind");
        close(server_fd);
        return -1;
    }

    /* take whichever is larger - BACKLOG is a floor, not a cap. No-op on a system whose
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
    /* in_buf stays NULL (calloc) - input memory is borrowed from App.read_buf per read and only
     * owned while a request is partially received (see Connection.in_buf). */
    conn->fd = fd;
    conn->file_fd = -1;
    conn->last_activity = time(NULL);
    /* request_started stays 0 (calloc) until a request actually starts arriving: a freshly accepted,
     * otherwise-silent connection is bounded by IDLE_TIMEOUT_SECONDS below, same as before (targets
     * a request that is under way but moving too slowly, not one that never starts). */
    conn->arena = &app->arena; /* shared per-worker arena, not one allocated per connection */
    conn->app = app;
    return conn;
}

/* ---- per-worker buffered-memory budget (ServerConfig.max_buffered_bytes) ---- */

/* Bytes conn owns across event-loop turns: a borrowed App.read_buf and an arena out_buf are not its own.
 * A pinned shared_body counts as a copy would: evicted from the cache, only this connection keeps it alive. */
static size_t owned_bytes(const App *app, const Connection *conn) {
    size_t n = 0;
    if (conn->in_buf != NULL && conn->in_buf != app->read_buf) {
        n += conn->in_cap;
    }
    if (conn->out_buf_owned) {
        n += conn->out_cap;
    }
    if (conn->stream_buf != NULL) {
        n += STREAM_CHUNK_SIZE;
    }
    if (conn->deferred != NULL) {
        /* its whole arena (the handed-over worker buffer + fallback blocks) and a held batch tail */
        n += ARENA_SIZE + conn->deferred->arena.large_bytes + conn->deferred->prefix_len;
    }
    return n + shared_body_left(conn);
}

/* Re-reads conn's ownership and moves App.buffered_bytes by the difference. Idempotent, so it is called
 * after every change of in_buf/out_buf_owned/stream_buf rather than paired with each malloc and free. */
static void sync_held_bytes(App *app, Connection *conn) {
    const size_t now = owned_bytes(app, conn);
    app->buffered_bytes = app->buffered_bytes - conn->held_bytes + now;
    conn->held_bytes = now;
}

/* 1 if the worker may hold `extra` more bytes. */
static int budget_allows(const App *app, const size_t extra) {
    return app->config.max_buffered_bytes == 0 || app->buffered_bytes + extra <= app->config.max_buffered_bytes;
}

/* ---- deferred-response handle table (App.defer_slots) ---- */

static DeferHandle make_handle(const uint32_t gen, const uint32_t slot) {
    return ((DeferHandle)gen << 32) | ((DeferHandle)slot + 1);
}

/* The live DeferredRequest `h` names, or NULL (0, out of range, or a generation that has moved on). */
static DeferredRequest *defer_lookup(const App *app, const DeferHandle h) {
    const uint64_t index = h & 0xFFFFFFFFu;
    if (app == NULL || index == 0 || index > app->defer_slots_cap) {
        return NULL;
    }
    const DeferSlot *const slot = &app->defer_slots[index - 1];
    return slot->dr != NULL && slot->gen == (uint32_t)(h >> 32) ? slot->dr : NULL;
}

/* Gives dr a free slot, growing the table by doubling (and App.defer_ready with it, so queueing a resumed
 * handle rarely needs to grow). Returns the slot index, or UINT32_MAX on OOM. */
static uint32_t defer_slot_take(App *app, DeferredRequest *dr) {
    if (app->defer_free_head == UINT32_MAX) {
        const uint32_t old_cap = app->defer_slots_cap;
        if (old_cap > UINT32_MAX / 4) {
            return UINT32_MAX;
        }
        const uint32_t cap = old_cap == 0 ? 64 : old_cap * 2;
        /* both freed by app_destroy */
        DeferSlot *const slots = realloc(app->defer_slots, (size_t)cap * sizeof(DeferSlot));
        if (slots == NULL) {
            return UINT32_MAX;
        }
        app->defer_slots = slots;
        if (app->defer_ready_cap < cap) {
            DeferHandle *const ready = realloc(app->defer_ready, (size_t)cap * sizeof(DeferHandle));
            if (ready == NULL) {
                return UINT32_MAX; /* the grown slot array is kept, unused past defer_slots_cap */
            }
            app->defer_ready = ready;
            app->defer_ready_cap = cap;
        }
        for (uint32_t i = old_cap; i < cap; i++) {
            slots[i].dr = NULL;
            slots[i].gen = 1;
            slots[i].next_free = i + 1 < cap ? i + 1 : UINT32_MAX;
        }
        app->defer_free_head = old_cap;
        app->defer_slots_cap = cap;
    }
    const uint32_t index = app->defer_free_head;
    app->defer_free_head = app->defer_slots[index].next_free;
    app->defer_slots[index].dr = dr;
    return index;
}

/* Ends dr's life: the slot is freed and its generation bumped (every handle to it goes stale), the held batch
 * tail is freed, the arena's fallback blocks are freed and its buffer goes back to App.arena_pool. dr itself
 * lives in that arena: nothing may touch it afterwards. Does not touch dr->conn. */
static void release_deferred(App *app, DeferredRequest *dr) {
    DeferSlot *const slot = &app->defer_slots[dr->slot];
    slot->dr = NULL;
    slot->gen = slot->gen + 1 == 0 ? 1 : slot->gen + 1;
    slot->next_free = app->defer_free_head;
    app->defer_free_head = dr->slot;
    free(dr->prefix);
    Arena kept = dr->arena; /* copied out first: releasing it frees the memory dr is in */
    arena_release_to_pool(&kept, &app->arena_pool);
}

void connection_close(App *app, Connection *conn) {
    if (conn == NULL) {
        return;
    }
    if (app != NULL) {
        app->buffered_bytes -= conn->held_bytes; /* everything it owns is freed below */
        conn->held_bytes = 0;
        event_loop_release_fd(app, conn->fd);
        if (app->connections != NULL && conn->fd >= 0 && conn->fd < app->connections_cap) {
            app->connections[conn->fd] = NULL;
        }
        app->open_connections--; /* paired with accept_connections' ++ */
        if (app->spare_fd < 0) {
            /* Opportunistic re-arm: this connection closing just freed a descriptor, the
             * cheapest possible moment to try reclaiming the reserve - waiting for the next
             * accept_connections call could be a long time if the listen socket is the thing
             * that's starved. Best effort: still -1 on failure, tried again next close. */
            app->spare_fd = open("/dev/null", O_RDONLY);
        }
    }

    if (conn->deferred != NULL && app != NULL) {
        /* the client left (or a deadline, write error, shutdown): the handle goes stale, res_resume returns NULL */
        DeferredRequest *const dr = conn->deferred;
        conn->deferred = NULL;
        release_deferred(app, dr);
    }
    if (conn->file_fd >= 0) {
        close(conn->file_fd);
        conn->file_fd = -1;
    }
    stream_release(conn); /* a producer stream cut short (peer gone, write error, shutdown) frees its ctx here */
    shared_body_detach(conn); /* the cache (or nobody, if it was evicted meanwhile) keeps the rest */
    /* stream_buf and a still-owned malloc'd out_buf tail-copy are connection-owned, unlike the
     * (shared, App-owned) arena a normal out_buf lives in - free them here regardless of which exit
     * path got the connection closed (a hard write/read error mid-response, not just the ordinary
     * "response fully sent, not keeping this connection alive" case). out_buf can equal stream_buf
     * (streaming's out_buf just points at it) - free it once, via stream_buf, never both. */
    if (conn->out_buf == conn->stream_buf) {
        conn->out_buf = NULL;
    } else if (conn->out_buf_owned) {
        free(conn->out_buf);
        conn->out_buf = NULL;
    }
    free(conn->stream_buf);
    conn->stream_buf = NULL;

    if (conn->fd >= 0) {
        close(conn->fd);
        conn->fd = -1;
    }
    if (app == NULL || conn->in_buf != app->read_buf) {
        free(conn->in_buf); /* owned (or NULL); App.read_buf is only ever borrowed, app_destroy frees it */
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
    /* the one shared arena every Connection.arena pointed at. Every connection above was already
     * closed (connection_close no longer touches app->arena itself), so nothing still references it. */
    arena_destroy(&app->arena);
    free(app->arena.buf);
    app->arena.buf = NULL;
    arena_pool_destroy(&app->arena_pool); /* spares only: every block handed out came back with its connection */
    free(app->read_buf); /* no connection is left to have borrowed it */
    app->read_buf = NULL;
    free(app->batch_buf);
    app->batch_buf = NULL;
    app->batch_len = 0;
    free(app->defer_slots); /* every connection is closed above, so no deferred request is left in it */
    app->defer_slots = NULL;
    app->defer_slots_cap = 0;
    app->defer_free_head = UINT32_MAX;
    free(app->defer_ready);
    app->defer_ready = NULL;
    app->defer_ready_len = 0;
    app->defer_ready_cap = 0;
    free(app->watched); /* the fds themselves belong to the application */
    app->watched = NULL;
    app->watched_cap = 0;
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

/* whether a response has been started for the current request (bytes built, a body pinned, a file or a
 * producer recorded). */
static int response_started(const Connection *conn) {
    return conn->out_buf != NULL || conn->shared_body != NULL || conn->file_fd >= 0 || conn->stream_fn != NULL;
}

/* whether a response is still being written (built but not fully queued on the socket). a
 * producer stream counts for its whole life, paused or not - it has not sent its last chunk yet. A deferred
 * request counts from res_defer until its response is written. */
static int response_pending(const Connection *conn) {
    return response_started(conn) || conn->deferred != NULL;
}

void app_stop(App *app) {
    if (app == NULL || app->is_shutting_down) {
        return;
    }
    app->is_shutting_down = 1;

    /* Stop accepting new connections: deregister from event loop and close server socket */
    if (app->server_fd >= 0) {
        if (event_loop_is_open(app)) {
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
            if (conn->in_len == 0 && !response_pending(conn)) {
                connection_close(app, conn);
            } else {
                conn->keep_alive = 0;
            }
        }
    }

    /* Arm a oneshot shutdown deadline timer on the event loop so slow/stalled clients
     * cannot prevent the server process from exiting indefinitely. */
    if (event_loop_is_open(app)) {
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
 * Overload shedding: writes a minimal, hand-built 503 (no Connection/arena/Response - this
 * happens before any of that would normally exist) to a freshly accepted fd, then closes it.
 * Best-effort and one nonblocking attempt only: a client that won't read even this gets no more
 * consideration than one that was never told anything, which is fine - the point is to answer
 * politely when possible, not to guarantee delivery under overload (that would need to hold and
 * retry the write, defeating the purpose of shedding cheaply).
 */
static void reject_overloaded_connection(int client_fd) {
    const char *body = status_text(503);
    char head[192];
    int head_len = snprintf(head, sizeof(head),
                             "HTTP/1.1 503 %s\r\nDate: %s\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n"
                             "Connection: close\r\n\r\n",
                             body, http_date_for(time(NULL)), strlen(body));
    /* client_fd is already non-blocking: accept_client, or a master's accept_client before an
     * SCM_RIGHTS handoff (this used to cost its own two fcntl calls). */
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
 * CEXPRESS_SINGLE_ACCEPTOR, a passed fd received over a control socket): the overload check,
 * table growth and Connection setup. Expects O_NONBLOCK/TCP_NODELAY to be in effect already: an fd
 * from accept_client has both (accept4 / inheritance from the listener, no per-fd syscalls); a
 * passed fd was accept_client'ed by the master and keeps both (POSIX: file status flags and socket
 * options are properties of the underlying open file description, not the fd number, so they carry
 * over across an SCM_RIGHTS handoff same as across dup()/fork()).
 */
static void admit_connection(App *app, int client_fd) {
    /* shed load past the configured cap instead of accumulating connections (and their
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
    app->open_connections++; /* paired with connection_close's -- */

    event_loop_watch_read(app, client_fd, conn);
}

void accept_connections(App *app) {
    while (1) {
        const int client_fd = accept_client(app->server_fd);
        if (client_fd < 0) {
            /* Out of descriptors process- or system-wide: without a free slot, accept() keeps
             * failing this way forever (nothing here releases one on its own), silently starving
             * every future connection - the old behavior. MEASURED (macOS/BSD): the connection
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
 * this worker's server_fd is a control socket (the worker-side end of a socketpair with the
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
 * a response could not be fully written this turn. Ask for write readiness and stop reading
 * until it drains: in_buf may already hold the next pipelined request, and serving it now would
 * build a new response over the one still pending. Level-triggered read readiness would otherwise
 * keep firing for bytes handle_readable must not consume yet. flush_connection's keep-alive branch
 * re-watches read once the response is fully queued (a closed connection needs neither).
 */
static void wait_for_writable(App *app, Connection *conn) {
    sync_held_bytes(app, conn); /* a tail copy or stream_buf may have just been taken */
    event_loop_watch_write(app, conn->fd, conn);
    if (conn->events_watched & EVENT_READ) {
        event_loop_unwatch_read(app, conn->fd);
    }
}

/*
 * a producer returned STREAM_PAUSE and its output has drained. Stop asking for writability (the
 * socket is writable, so a level-triggered write watch would call the producer in a busy loop) and keep
 * read interest only so read_and_serve can notice the peer hanging up (watch_stream_peer) while
 * nothing is being written. app_wake_streams / close_idle_connections resume it (resume_stream).
 */
static int park_stream(App *app, Connection *conn) {
    sync_held_bytes(app, conn); /* a parked stream keeps its stream_buf */
    event_loop_unwatch_write(app, conn->fd, conn);
    if (!(conn->events_watched & EVENT_READ)) {
        event_loop_watch_read(app, conn->fd, conn);
    }
    return FLUSH_PENDING;
}

/* un-parks a paused producer stream: its next call happens on the next write-readiness event. The
 * write-stall clock restarts now - time spent paused is the producer's choice, not a stalled reader. */
static void resume_stream(App *app, Connection *conn) {
    conn->stream_paused = 0;
    conn->last_write_progress = time(NULL);
    event_loop_watch_write(app, conn->fd, conn);
}

/* nothing is buffered any more - drop the input memory so an idle connection holds none. An
 * owned buffer is freed; a borrowed App.read_buf is just handed back. */
static void release_in_buf(App *app, Connection *conn) {
    if (conn->in_buf != app->read_buf) {
        free(conn->in_buf);
    }
    conn->in_buf = NULL;
    conn->in_cap = 0;
    conn->in_len = 0;
    conn->in_off = 0;
    sync_held_bytes(app, conn);
}

/*
 * The socket is full (EAGAIN) mid-response: return to the event loop until it is writable again.
 * out_buf is shared-arena-resident unless it's already a connection-owned copy (out_buf_owned) or the
 * file-streaming chunk buffer (out_buf == stream_buf, never arena to begin with - see stream_buf's own
 * comment). Returning now would let another connection's dispatch reset and reuse the shared arena
 * before this response finishes draining, so its unsent part is copied out first; with nothing unsent,
 * out_buf just stops pointing into the arena. A shared_body or a file is not copied: this connection's
 * reference / fd keeps it. FLUSH_PENDING, or FLUSH_CLOSED (over budget or out of memory).
 */
static int keep_unsent_and_wait(App *app, Connection *conn) {
    if (conn->out_buf != NULL && !conn->out_buf_owned && conn->out_buf != conn->stream_buf) {
        const size_t out_left = conn->out_len - conn->out_sent;
        if (!budget_allows(app, out_left + shared_body_left(conn))) {
            /* over the worker's buffered-memory budget: this client is the one that just
             * stopped reading, so it is dropped rather than every other connection. */
            connection_close(app, conn);
            return FLUSH_CLOSED;
        }
        char *tail = NULL;
        if (out_left > 0) {
            tail = malloc(out_left); /* freed once drained (flush_connection) or by connection_close */
            if (tail == NULL) {
                connection_close(app, conn);
                return FLUSH_CLOSED;
            }
            memcpy(tail, conn->out_buf + conn->out_sent, out_left);
        }
        conn->out_buf = tail;
        conn->out_len = out_left;
        conn->out_sent = 0;
        conn->out_cap = out_left;
        conn->out_buf_owned = tail != NULL;
    }
    wait_for_writable(app, conn);
    return FLUSH_PENDING;
}

/* out_buf is fully written and a file body follows: drop it (free a tail copy; an arena one is just
 * forgotten) so nothing points into the arena across turns and no owned copy leaks. */
static void release_drained_out_buf(Connection *conn) {
    if (conn->out_buf_owned) {
        free(conn->out_buf);
        conn->out_buf_owned = 0;
    }
    if (conn->out_buf != conn->stream_buf) {
        conn->out_buf = NULL;
        conn->out_len = 0;
        conn->out_sent = 0;
        conn->out_cap = 0;
    }
}

/*
 * Puts the coalesced responses in App.batch_buf in front of conn's unsent out_buf, so they go out in the
 * same write (and in order). The joined bytes stay in batch_buf when they fit, else in one exact-size arena
 * block; either way out_buf is not owned, so an EAGAIN copies the unsent part out like any arena response.
 * batch_len is 0 afterwards. Returns 0, or -1 once conn has been closed (out of memory).
 */
static int take_batch(App *app, Connection *conn) {
    const size_t batch = app->batch_len;
    const size_t out_left = conn->out_buf != NULL ? conn->out_len - conn->out_sent : 0;
    const size_t total = batch + out_left;
    app->batch_len = 0;
    char *joined = app->batch_buf;
    if (total > BATCH_BUF_SIZE) {
        joined = arena_alloc(conn->arena, total); /* reclaimed by the caller's arena_reset */
        if (joined == NULL) {
            connection_close(app, conn);
            return -1;
        }
        memcpy(joined, app->batch_buf, batch);
    }
    if (out_left > 0) {
        memcpy(joined + batch, conn->out_buf + conn->out_sent, out_left);
    }
    if (conn->out_buf_owned) {
        free(conn->out_buf);
        conn->out_buf_owned = 0;
    }
    conn->out_buf = joined;
    conn->out_len = total;
    conn->out_sent = 0;
    conn->out_cap = total;
    return 0;
}

/*
 * The response for the request at in_off is fully queued (written, or coalesced into App.batch_buf) on a
 * keep-alive connection: consume that request (request_len bytes; 0 = nothing left to consume, e.g. a
 * coalesced batch flushed on its own, whose requests were consumed as they were queued) and reset the
 * per-request state for the next one. Event-loop interest is the caller's business.
 */
static void finish_request(App *app, Connection *conn) {
    const size_t consumed = conn->request_len;
    conn->request_len = 0;
    if (conn->in_off + consumed >= conn->in_len) {
        conn->in_len = 0;
        conn->in_off = 0;
    } else {
        conn->in_off += consumed;
    }
    /* Back to idle between requests - unless pipelined bytes are already waiting, which
     * start the next request's clock now. */
    conn->request_started = conn->in_len > 0 ? time(NULL) : 0;
    conn->last_write_progress = 0; /* no response pending: WRITE_TIMEOUT_SECONDS stops applying */
    conn->body_limit_checked = 0; /* next request on this connection gets its own body-limit check */
    conn->continue_sent = 0; /* ... and its own 100 Continue */
    conn->chunk_scan = (ChunkScanState){0}; /* next request's chunked body scans from its own start */
    conn->head_scan = 0; /* ... and its head's blank-line search too */

    /* nothing buffered - free the input buffer (however far it grew for a large body) or hand
     * App.read_buf back, so an idle keep-alive connection owns no input memory. Pipelined
     * leftovers keep the buffer as it is. */
    if (conn->in_len == 0) {
        release_in_buf(app, conn);
    }
}

int flush_connection(App *app, Connection *conn) {
    size_t bytes_written_this_flush = 0;
    const size_t max_flush_bytes = 4 * STREAM_CHUNK_SIZE;

    if (app->batch_len > 0 && take_batch(app, conn) != 0) {
        return FLUSH_CLOSED;
    }

    if (conn->last_write_progress == 0) {
        /* First time flush_connection runs for this response: start the write-stall clock now, even
         * before the first byte actually goes out (a response that gets EAGAIN on every attempt is
         * exactly the "made no progress" case the deadline exists for). */
        conn->last_write_progress = time(NULL);
    }

    while (1) {
        /* out_buf first, then a pinned shared_body (res_send_shared) straight from its bytes: one writev
         * for both, so a cached static file costs one syscall and no copy of its body. */
        while ((conn->out_sent < conn->out_len && !head_rides_sendfile(conn)) || conn->shared_body != NULL) {
            const size_t out_left = conn->out_len - conn->out_sent;
            ssize_t n;
            if (conn->shared_body == NULL) {
                n = conn_write(conn, conn->out_buf + conn->out_sent, out_left);
            } else {
                struct iovec iov[2];
                int iovcnt = 0;
                if (out_left > 0) {
                    iov[iovcnt].iov_base = conn->out_buf + conn->out_sent;
                    iov[iovcnt].iov_len = out_left;
                    iovcnt++;
                }
                iov[iovcnt].iov_base = conn->shared_body->data + conn->shared_body_sent;
                iov[iovcnt].iov_len = shared_body_left(conn);
                iovcnt++;
                n = conn_writev(conn, iov, iovcnt);
            }
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return keep_unsent_and_wait(app, conn);
                }
                connection_close(app, conn);
                return FLUSH_CLOSED;
            }
            const size_t out_part = (size_t)n < out_left ? (size_t)n : out_left;
            conn->out_sent += out_part;
            if (conn->shared_body != NULL) {
                conn->shared_body_sent += (size_t)n - out_part;
                if (conn->shared_body_sent == conn->shared_body->len) {
                    shared_body_detach(conn); /* last byte written: the cache (if still holding it) keeps it */
                }
            }
            conn->last_activity = time(NULL);
            conn->last_write_progress = conn->last_activity; /* only advances on actual bytes written */
            bytes_written_this_flush += (size_t)n;
        }

        /* The current out_buf has been fully drained to the socket (or, on macOS, rides in front of the
         * file in the sendfile call below) */
        if (conn->file_fd >= 0) {
            if (conn->file_remaining > 0 && !conn->file_no_sendfile) {
                const size_t head_left = conn->out_len - conn->out_sent; /* > 0 only when it rides sendfile */
                if (head_left == 0) {
                    release_drained_out_buf(conn);
                }
                if (bytes_written_this_flush >= max_flush_bytes) {
                    wait_for_writable(app, conn); /* same fairness yield as the pread path below */
                    return FLUSH_PENDING;
                }
                const size_t want = conn->file_remaining < max_flush_bytes ? conn->file_remaining : max_flush_bytes;
                size_t sent = 0;
                const int rc = conn_sendfile(conn, head_left > 0 ? conn->out_buf + conn->out_sent : NULL,
                                             head_left, want, &sent);
                const int err = rc != 0 ? errno : 0;
                const int blocked = err == EAGAIN || err == EWOULDBLOCK;
                if (rc != 0 && !blocked) {
                    if (sent == 0 && sendfile_unsupported(err)) {
                        conn->file_no_sendfile = 1; /* this fd pair: pread + write from here on */
                        continue;
                    }
                    connection_close(app, conn);
                    return FLUSH_CLOSED;
                }
                const size_t head_part = sent < head_left ? sent : head_left;
                const size_t file_part = sent - head_part;
                conn->out_sent += head_part;
                conn->file_offset += (off_t)file_part;
                conn->file_remaining -= file_part;
                if (sent > 0) {
                    conn->last_activity = time(NULL);
                    conn->last_write_progress = conn->last_activity;
                    bytes_written_this_flush += sent;
                }
                if (blocked) {
                    return keep_unsent_and_wait(app, conn);
                }
                if (file_part == 0) {
                    /* no error, no file bytes: the file shrank since res_send_file's fstat (EOF) - the
                     * promised Content-Length can no longer be met, so end the connection */
                    connection_close(app, conn);
                    return FLUSH_CLOSED;
                }
                continue;
            }
            if (conn->file_remaining > 0) {
                if (bytes_written_this_flush >= max_flush_bytes) {
                    /* Yield to event loop to share bandwidth fairly */
                    wait_for_writable(app, conn);
                    return FLUSH_PENDING;
                }

                size_t to_read = conn->file_remaining < STREAM_CHUNK_SIZE
                                     ? conn->file_remaining
                                     : STREAM_CHUNK_SIZE;
                /* a connection-owned buffer, malloc'd once and reused chunk to chunk - never the
                 * shared arena, since a large file spans many event-loop turns during which other
                 * connections would otherwise reuse and overwrite it (see Connection.stream_buf). A
                 * still-owned malloc'd tail-copy from the response head (above) is done with once we
                 * get here (the inner loop just fully drained it) and isn't stream_buf, so free it now
                 * rather than leak it when out_buf moves on to stream_buf below. */
                if (conn->out_buf_owned) {
                    free(conn->out_buf);
                    conn->out_buf_owned = 0;
                }
                if (conn->stream_buf == NULL) {
                    conn->stream_buf = malloc(STREAM_CHUNK_SIZE);
                    if (conn->stream_buf == NULL) {
                        connection_close(app, conn);
                        return FLUSH_CLOSED;
                    }
                }
                conn->out_buf = conn->stream_buf;
                /* pread at file_offset: the sendfile path (which may have run first) never moves the
                 * file position */
                ssize_t r = pread(conn->file_fd, conn->out_buf, to_read, conn->file_offset);
                if (r <= 0) {
                    connection_close(app, conn);
                    return FLUSH_CLOSED;
                }
                conn->out_len = (size_t)r;
                conn->out_sent = 0;
                conn->out_cap = STREAM_CHUNK_SIZE;
                conn->file_offset += (off_t)r;
                conn->file_remaining -= (size_t)r;
                continue;
            } else {
                close(conn->file_fd);
                conn->file_fd = -1;
                release_drained_out_buf(conn); /* a sendfile-path head tail copy, already written */
                free(conn->stream_buf);
                conn->stream_buf = NULL;
                conn->out_buf = NULL;
                conn->out_cap = 0;
            }
        }

        /* a producer stream - the previous turn's output has drained, ask for the next one. */
        if (conn->stream_fn != NULL) {
            if (conn->stream_paused) {
                return park_stream(app, conn);
            }
            if (bytes_written_this_flush >= max_flush_bytes) {
                wait_for_writable(app, conn); /* same fairness yield as file streaming */
                return FLUSH_PENDING;
            }
            if (conn->out_buf_owned) {
                /* the response head's tail-copy (see the file branch above): drained, done with */
                free(conn->out_buf);
                conn->out_buf_owned = 0;
            }
            if (conn->stream_buf == NULL) {
                conn->stream_buf = malloc(STREAM_CHUNK_SIZE);
                if (conn->stream_buf == NULL) {
                    connection_close(app, conn);
                    return FLUSH_CLOSED;
                }
            }
            /* cap leaves room for the 5-byte last chunk, so STREAM_END below never overflows */
            StreamWriter writer = { conn->stream_buf, 0, STREAM_CHUNK_SIZE - 5 };
            const int step = conn->stream_fn(&writer, conn->stream_ctx);
            if (step != STREAM_MORE && step != STREAM_PAUSE && step != STREAM_END) {
                connection_close(app, conn); /* STREAM_ABORT (or garbage): truncate, never fake an ending */
                return FLUSH_CLOSED;
            }
            if (step == STREAM_END) {
                memcpy(writer.buf + writer.len, "0\r\n\r\n", 5);
                writer.len += 5;
                stream_release(conn); /* ctx freed now; the loop drains the tail and finishes below */
            } else if (step == STREAM_PAUSE || writer.len == 0) {
                conn->stream_paused = 1; /* parked once this turn's bytes (if any) have drained */
            }
            conn->out_buf = conn->stream_buf;
            conn->out_len = writer.len;
            conn->out_sent = 0;
            conn->out_cap = STREAM_CHUNK_SIZE;
            continue;
        }

        break;
    }

    /* Whatever is left in out_buf now has been fully drained (the inner loop only exits via that, or
     * the streaming branch above, which never leaves it non-NULL and un-freed). A connection-owned
     * tail-copy needs freeing here; an arena-resident buffer does not (the caller - handle_readable
     * or reject_request - resets the shared arena once this whole dispatch-and-flush cycle is done). */
    if (conn->out_buf_owned) {
        free(conn->out_buf);
        conn->out_buf_owned = 0;
    }
    if (conn->stream_buf != NULL) {
        /* a producer stream just ended (its last chunk is what drained): release the turn buffer */
        if (conn->out_buf == conn->stream_buf) {
            conn->out_buf = NULL;
        }
        free(conn->stream_buf);
        conn->stream_buf = NULL;
    }

    if (conn->keep_alive) {
        /* Drop write registration from a partial write above, and resume reading (reads pause
         * while a response is pending - see wait_for_writable). */
        event_loop_unwatch_write(app, conn->fd, conn);
        if (!(conn->events_watched & EVENT_READ)) {
            event_loop_watch_read(app, conn->fd, conn);
        }
        conn->out_buf = NULL;
        conn->out_len = 0;
        conn->out_sent = 0;
        conn->out_cap = 0;
        /* keep whatever follows the request just answered (a pipelined next request, whole or
         * partial) instead of discarding the buffer. */
        finish_request(app, conn);
        sync_held_bytes(app, conn); /* tail copy and stream_buf are freed above */
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
    /* conn may already be freed (flush_connection always closes here, keep_alive is forced off
     * above) - reset the shared arena through app, never conn, once this dispatch-and-flush cycle
     * that just used it is over. */
    arena_reset(&app->arena);
}

/*
 * in_buf is full with headers complete: grow it to fit the body (a borrowed App.read_buf is
 * copied into a new owned buffer instead of realloc'd). Returns 1 grown (keep reading),
 * 0 realloc failed (-> 500), -1 chunked raw-size cap hit (-> 413), -2 the growth would exceed the
 * worker's ServerConfig.max_buffered_bytes (-> 503).
 * Both Content-Length and chunked grow the same way: doubling, capped at the known target size
 * (header_len + content_length + 1, or header_len + the route's body limit for chunked's raw wire size) - never
 * one realloc straight to the full size a client merely *declared*. A client that sends a 10 MiB
 * Content-Length and then only a few KB of body costs a reservation proportional to what actually
 * arrived (doubling from BUF_SIZE), not the declared 10 MiB up front; the full target is only reached
 * once that much has genuinely been received (and buffered) across repeated calls here.
 * A Content-Length above MAX_BODY_SIZE never gets here: request_is_complete already stopped
 * buffering and parse_http_request answered 413. A route-specific limit below MAX_BODY_SIZE
 * (app_use_body_limit) is enforced earlier still, in handle_readable, before this is ever called.
 */
/* The next in_cap when a body outgrows in_buf: doubling, but at least BUF_SIZE, so a partial request's
 * small owned buffer (IN_BUF_GRANULE-rounded, stop_borrowing_read_buf) does not crawl up through
 * 1 KiB, 2 KiB, ... once a body is arriving. */
static size_t next_in_cap(const size_t in_cap) {
    return in_cap < BUF_SIZE ? BUF_SIZE : in_cap * 2;
}

/* A header block filled a small owned in_buf (below BUF_SIZE, see stop_borrowing_read_buf) before its
 * blank line: double it, up to BUF_SIZE, the header limit - a full BUF_SIZE buffer with no blank line is
 * still the only 431. Returns 1 grown, 0 realloc failed (-> 500), -2 over max_buffered_bytes (-> 503). */
static int grow_head_buf(App *app, Connection *conn) {
    const size_t needed = conn->in_cap * 2 > BUF_SIZE ? BUF_SIZE : conn->in_cap * 2;
    if (!budget_allows(app, needed - conn->in_cap)) {
        return -2;
    }
    char *grown = realloc(conn->in_buf, needed); /* owned: a borrowed App.read_buf is always BUF_SIZE */
    if (grown == NULL) {
        return 0;
    }
    conn->in_buf = grown;
    conn->in_cap = needed;
    sync_held_bytes(app, conn);
    return 1;
}

static int grow_in_buf(App *app, Connection *conn, const size_t header_len, const int chunked,
                       const int content_length) {
    size_t needed;
    if (chunked) {
        /* the route's own limit, not MAX_BODY_SIZE: raw wire bytes are never fewer than decoded ones */
        const size_t raw_cap = header_len + (conn->body_limit_checked ? conn->body_limit : MAX_BODY_SIZE);
        if (conn->in_cap >= raw_cap) {
            return -1;
        }
        needed = next_in_cap(conn->in_cap) > raw_cap ? raw_cap : next_in_cap(conn->in_cap);
    } else if (content_length >= 0) {
        const size_t target = header_len + (size_t)content_length + 1;
        if (conn->in_cap >= target) {
            return 1; /* already large enough (request_is_complete will pick this up next read) */
        }
        needed = next_in_cap(conn->in_cap) > target ? target : next_in_cap(conn->in_cap);
    } else {
        return 0;
    }
    const size_t held_now = conn->in_buf == app->read_buf ? 0 : conn->in_cap;
    if (!budget_allows(app, needed - held_now)) {
        return -2;
    }
    char *grown;
    if (conn->in_buf == app->read_buf) {
        /* a borrowed App.read_buf is never realloc'd - move to an owned buffer of the grown size. */
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
    sync_held_bytes(app, conn);
    return 1;
}

/*
 * as soon as a request's headers are complete, look up its app_use_body_limit (on the canonical
 * path - request_target_path - so a query string, "//" or percent-encoding cannot dodge the prefix)
 * into conn->body_limit, and reject a declared Content-Length over it with 413 - before any body
 * buffering happens. Runs at most once per request (conn->body_limit_checked). A chunked body is
 * held to the same limit by reject_if_chunked_over_body_limit and grow_in_buf.
 * Takes an already-parsed head instead of running its own request_framing pass over conn->in_buf.
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
    conn->body_limit = MAX_BODY_SIZE;
    if (head->content_length < 0) {
        return 0; /* malformed or already oversized globally: the full parse answers it */
    }
    conn->body_limit = app_body_limit_for_target(app, head->path, head->path_len);
    if (!head->chunked && (size_t)head->content_length > conn->body_limit) {
        reject_request(app, conn, 413);
        return 1;
    }
    return 0;
}

/*
 * a chunked body whose validated chunks (conn->chunk_scan, just advanced by
 * request_head_is_complete) already decode past the request's body limit gets 413 - whether or not
 * the rest of the body has arrived. Returns 1 if rejected (conn closed), 0 otherwise.
 */
static int reject_if_chunked_over_body_limit(App *app, Connection *conn, const ParsedHead *head) {
    if (!head->chunked || !conn->body_limit_checked || conn->chunk_scan.decoded_len <= conn->body_limit) {
        return 0;
    }
    reject_request(app, conn, 413);
    return 1;
}

/*
 * the request's headers are complete, its body is not, and it carries "Expect: 100-continue"
 * (request_head_expects_continue): write "HTTP/1.1 100 Continue" once so the client sends the body now
 * instead of after its own fallback timeout (curl: 1 s). Runs after reject_if_over_body_limit, so a
 * body over the route or global limit gets its 413 instead and the body is never invited. Written
 * straight to the socket, not through out_buf: serve_buffered_requests only gets here with no response
 * pending, so nothing can be queued ahead of it. EAGAIN (nothing written) is harmless - the client falls
 * back to its timeout, as before 100-continue support. A short write would leave half a status line ahead of the final
 * response, so it closes the connection like a hard write error.
 * Returns 0 (conn open), or -1 once conn has been closed (freed).
 */
static int send_continue_if_expected(App *app, Connection *conn, const ParsedHead *head) {
    if (conn->continue_sent || !request_head_expects_continue(head)) {
        return 0;
    }
    conn->continue_sent = 1;
    static const char CONTINUE_LINE[] = "HTTP/1.1 100 Continue\r\n\r\n";
    const size_t len = sizeof(CONTINUE_LINE) - 1;
    const ssize_t n = conn_write(conn, CONTINUE_LINE, len);
    if (n == (ssize_t)len || (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))) {
        return 0;
    }
    connection_close(app, conn);
    return -1;
}

/* serve_buffered_requests results. */
#define SERVE_NEED_MORE 0
#define SERVE_WAIT 1
#define SERVE_CLOSED -1

/* moves the unserved tail of in_buf (from in_off) to the front. Called only when more input has
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
 * 1 if the response just built for the request at in_off can wait in App.batch_buf instead of being
 * written now: another request is already buffered behind it (a batch of one is written as before),
 * it is not the last one this serve call may dispatch (so the batch never outlives the loop), the
 * connection stays open, and the response is plain bytes in out_buf that fit what is left of the
 * batch (a file, shared_body or producer stream is written by flush_connection, batch in front).
 */
static int can_coalesce(const App *app, const Connection *conn, const int served) {
    return app->batch_buf != NULL && conn->keep_alive && served + 1 < MAX_PIPELINED_PER_EVENT &&
           conn->in_off + conn->request_len < conn->in_len && conn->out_buf != NULL && !conn->out_buf_owned &&
           conn->out_sent == 0 && conn->shared_body == NULL && conn->file_fd < 0 && conn->stream_fn == NULL &&
           conn->out_len <= BATCH_BUF_SIZE - app->batch_len;
}

/*
 * Writes the coalesced responses in App.batch_buf on their own (the serve loop stopped with no response
 * of its own to carry them). Their requests were consumed as they were queued, so request_len 0: the
 * keep-alive reset consumes nothing. A partial write keeps the rest as conn's pending response, exactly
 * as for any response (FLUSH_PENDING). FLUSH_DONE with nothing queued.
 */
static int flush_batch(App *app, Connection *conn) {
    if (app->batch_len == 0) {
        return FLUSH_DONE;
    }
    conn->request_len = 0;
    return flush_connection(app, conn);
}

/* ---- deferred responses (res_defer / res_resume) ---- */

static int serve_buffered_requests(App *app, Connection *conn, int *served_out);

DeferHandle res_defer(Response *res) {
    Connection *const conn = res->conn;
    App *const app = conn != NULL ? conn->app : NULL;
    if (app == NULL || conn->deferred != NULL || response_started(conn) || conn->arena != &app->arena) {
        return 0; /* not an engine connection, already deferred, or already answered: nothing changes */
    }
    /* dr is allocated in the worker arena before the hand-over, so it moves with everything else the handler
     * allocated and lives inside its own arena from then on */
    DeferredRequest *const dr = budget_allows(app, ARENA_SIZE + conn->request_len)
                                    ? arena_alloc(&app->arena, sizeof(DeferredRequest))
                                    : NULL;
    char *const block = dr != NULL ? arena_pool_take(&app->arena_pool) : NULL;
    const uint32_t slot = block != NULL ? defer_slot_take(app, dr) : UINT32_MAX;
    if (slot == UINT32_MAX) {
        arena_pool_give(&app->arena_pool, block); /* NULL is a no-op */
        res_status(res, 503);
        res_send(res, status_text(503));
        return 0;
    }
    arena_hand_over(&app->arena, &dr->arena, block, ARENA_SIZE);
    dr->conn = conn;
    dr->req = NULL;
    dr->res = res;
    dr->prefix = NULL;
    dr->prefix_len = 0;
    dr->deadline = time(NULL) + DEFER_TIMEOUT_SECONDS;
    dr->slot = slot;
    dr->state = DEFER_IN_HANDLER;
    conn->deferred = dr;
    conn->arena = &dr->arena; /* every res_* from now on builds into the kept arena */
    return make_handle(app->defer_slots[slot].gen, slot);
}

/* Queues h for serve_resumed. 0, or -1 when the queue cannot grow (the caller falls back to write readiness). */
static int queue_resumed(App *app, const DeferHandle h) {
    if (app->defer_ready_len == app->defer_ready_cap) {
        const size_t cap = app->defer_ready_cap == 0 ? 64 : app->defer_ready_cap * 2;
        DeferHandle *const grown = realloc(app->defer_ready, cap * sizeof(DeferHandle)); /* freed by app_destroy */
        if (grown == NULL) {
            return -1;
        }
        app->defer_ready = grown;
        app->defer_ready_cap = cap;
    }
    app->defer_ready[app->defer_ready_len++] = h;
    return 0;
}

Response *res_resume(App *app, DeferHandle h) {
    DeferredRequest *const dr = defer_lookup(app, h);
    if (dr == NULL) {
        return NULL;
    }
    if (dr->state == DEFER_WAITING && queue_resumed(app, h) != 0) {
        /* no room to queue it: handle_writable finishes a ready deferred response instead */
        event_loop_watch_write(app, dr->conn->fd, dr->conn);
    }
    dr->state = DEFER_READY; /* inside its own handler: serve_buffered_requests writes it after the handler */
    return dr->res;
}

const Request *req_deferred(App *app, DeferHandle h) {
    const DeferredRequest *const dr = defer_lookup(app, h);
    return dr != NULL ? dr->req : NULL;
}

/*
 * The handler returned with its request deferred and nothing sent. Copies the request's wire bytes, the
 * Request and the Response into the deferred arena, consumes the request from in_buf (pipelined requests
 * behind it stay buffered, unserved while it is pending), writes the responses coalesced before it (with
 * keep-alive: they belong to earlier keep-alive requests), and leaves only read interest, to notice a hang-up.
 * If the socket does not take that batch, its unsent tail moves to dr->prefix and goes out in front of this
 * response, so nothing is pending on the socket while deferred. SERVE_WAIT, or SERVE_CLOSED (conn freed).
 */
static int park_deferred(App *app, Connection *conn, const Request *req, const Response *res) {
    DeferredRequest *const dr = conn->deferred;
    const size_t wire_len = conn->request_len;
    char *const wire = arena_alloc(&dr->arena, wire_len + 1);
    Request *const kept_req = arena_alloc(&dr->arena, sizeof(Request));
    Response *const kept_res = arena_alloc(&dr->arena, sizeof(Response));
    if (wire == NULL || kept_req == NULL || kept_res == NULL) {
        app->batch_len = 0; /* the earlier responses are dropped with the connection */
        connection_close(app, conn);
        return SERVE_CLOSED;
    }
    const char *const req_start = conn->in_buf + conn->in_off;
    memcpy(wire, req_start, wire_len);
    request_clone_used(kept_req, req, req_start, wire_len, wire, &dr->arena);
    kept_req->body[kept_req->content_length] = '\0'; /* in the original that byte was handed back to the next request */
    response_clone_used(kept_res, res);
    dr->req = kept_req;
    dr->res = kept_res;
    dr->state = DEFER_WAITING;

    conn->in_off += wire_len;
    conn->request_len = 0; /* the keep-alive reset that finishes this response consumes nothing more */
    if (conn->in_off >= conn->in_len) {
        release_in_buf(app, conn);
    }
    sync_held_bytes(app, conn);

    if (app->batch_len > 0) {
        const int keep_alive = conn->keep_alive;
        conn->keep_alive = 1;
        const int flushed = flush_batch(app, conn);
        if (flushed == FLUSH_CLOSED) {
            return SERVE_CLOSED;
        }
        conn->keep_alive = keep_alive;
        if (flushed == FLUSH_PENDING && conn->out_buf_owned) {
            const size_t left = conn->out_len - conn->out_sent;
            memmove(conn->out_buf, conn->out_buf + conn->out_sent, left);
            dr->prefix = conn->out_buf; /* ownership moves: freed by finish (joined) or release_deferred */
            dr->prefix_len = left;
        }
        conn->out_buf = NULL;
        conn->out_buf_owned = 0;
        conn->out_len = 0;
        conn->out_sent = 0;
        conn->out_cap = 0;
        conn->last_write_progress = 0;
        sync_held_bytes(app, conn);
    }
    event_loop_unwatch_write(app, conn->fd, conn);
    if (!(conn->events_watched & EVENT_READ)) {
        event_loop_watch_read(app, conn->fd, conn);
    }
    return SERVE_WAIT;
}

/*
 * The deferred response on conn is ready to write: answers 500 if nothing was built, puts a held batch tail
 * in front, then detaches dr (conn->deferred = NULL, conn->arena back to the worker arena). The response
 * bytes stay in dr's arena until the caller's flush_connection has written them or copied the unsent tail;
 * the caller then release_deferred's dr. NULL if conn had to be closed (out of memory; dr released with it).
 */
static DeferredRequest *detach_deferred(App *app, Connection *conn) {
    DeferredRequest *const dr = conn->deferred;
    if (!response_started(conn)) {
        res_status(dr->res, 500); /* resumed but never answered */
        res_send(dr->res, status_text(500));
    }
    if (dr->prefix != NULL) {
        const size_t out_left = conn->out_buf != NULL ? conn->out_len - conn->out_sent : 0;
        const size_t total = dr->prefix_len + out_left;
        char *const joined = arena_alloc(&dr->arena, total);
        if (joined == NULL) {
            connection_close(app, conn);
            return NULL;
        }
        memcpy(joined, dr->prefix, dr->prefix_len);
        if (out_left > 0) {
            memcpy(joined + dr->prefix_len, conn->out_buf + conn->out_sent, out_left);
        }
        free(dr->prefix);
        dr->prefix = NULL;
        dr->prefix_len = 0;
        conn->out_buf = joined;
        conn->out_len = total;
        conn->out_sent = 0;
        conn->out_cap = total;
    }
    conn->deferred = NULL;
    conn->arena = &conn->app->arena;
    return dr;
}

/* Writes a ready deferred response, releases it, and serves the requests pipelined behind it. Only called
 * from the event loop's top level (serve_resumed, handle_writable, close_idle_connections): no serve loop of
 * another connection is running, App.batch_buf is empty and App.arena holds nothing. */
static void finish_deferred(App *app, DeferredRequest *dr) {
    Connection *const conn = dr->conn;
    if (detach_deferred(app, conn) == NULL) {
        return;
    }
    const int flushed = flush_connection(app, conn);
    release_deferred(app, dr);
    arena_reset(&app->arena);
    if (flushed == FLUSH_DONE && conn->in_len > conn->in_off) {
        int served;
        serve_buffered_requests(app, conn, &served);
    }
}

void serve_resumed(App *app) {
    /* by index: a handler run below may resume another request, which appends (and may realloc) */
    for (size_t i = 0; i < app->defer_ready_len; i++) {
        DeferredRequest *const dr = defer_lookup(app, app->defer_ready[i]);
        if (dr != NULL && dr->state == DEFER_READY) {
            finish_deferred(app, dr);
        }
    }
    app->defer_ready_len = 0;
}

/*
 * serves every complete request buffered in in_buf[in_off..in_len), in order, one response each,
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
            const int flushed = flush_batch(app, conn);
            if (flushed != FLUSH_DONE) {
                return flushed == FLUSH_CLOSED ? SERVE_CLOSED : SERVE_WAIT;
            }
            conn->in_off = 0;
            conn->in_len = 0;
            return SERVE_NEED_MORE;
        }
        char *req_start = conn->in_buf + conn->in_off;
        const size_t req_avail = conn->in_len - conn->in_off;

        /* One phr_parse_request pass, reused below by the body-limit check, the completeness check
         * and the full parse - these used to each run their own independent pass over the same
         * bytes, up to four per request once the body-limit check added its own. */
        ParsedHead head;
        parse_request_head_resume(req_start, req_avail, &head, &conn->head_scan);

        if (reject_if_over_body_limit(app, conn, &head)) {
            return SERVE_CLOSED;
        }

        const int complete = request_head_is_complete(&head, req_start, req_avail, &conn->chunk_scan);
        if (reject_if_chunked_over_body_limit(app, conn, &head)) {
            return SERVE_CLOSED;
        }
        if (!complete) {
            /* the coalesced responses go out before anything else is written (a 100 Continue) */
            const int flushed = flush_batch(app, conn);
            if (flushed != FLUSH_DONE) {
                return flushed == FLUSH_CLOSED ? SERVE_CLOSED : SERVE_WAIT;
            }
            if (send_continue_if_expected(app, conn, &head) != 0) {
                return SERVE_CLOSED;
            }
            compact_in_buf(conn);
            return SERVE_NEED_MORE;
        }

        Request req;
        /* req.body is a view into in_buf (no copy); body_saved is the byte its NUL replaced. */
        char body_saved = '\0';
        const int parse_status = parse_http_request_in_place(req_start, req_avail, &head, &req, conn->arena,
                                                             &body_saved);
        if (parse_status != 0) {
            /* -2: path too long (414). -3 is retired: req->headers holds views now, so
             * there is no fixed-size copy left to overflow - parse_http_request_from_head never
             * returns it any more (this is parse_status's own -3; req.content_length's -3 below is
             * an unrelated sentinel in a different code space). -4: a percent-decoded path/query
             * name/query value contained an embedded NUL, never silently truncated (400) - falls
             * into "anything else" below along with -1 (malformed), since both are already 400.
             * req.content_length == -2: body over MAX_BODY_SIZE (413), for both Content-Length and
             * chunked framing. req.content_length == -3: Transfer-Encoding names a coding this
             * engine doesn't implement, or "chunked" isn't its sole token (501, "Not Implemented" -
             * the server understood the request but can't process that transfer-coding). Anything
             * else: 400. */
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
        /* the body's NUL may sit on the first byte of a pipelined next request - put it back now
         * that the handler is done (res_* copied anything it sent), before flush_connection can free
         * in_buf or the next iteration parses from it. */
        req.body[req.content_length] = body_saved;

        /* res_defer: park it unless the handler also answered (res_resume or a res_* send before returning) */
        DeferredRequest *answered_deferred = NULL;
        if (conn->deferred != NULL) {
            if (conn->deferred->state == DEFER_IN_HANDLER && !response_started(conn)) {
                const int parked = park_deferred(app, conn, &req, &res);
                arena_reset(&app->arena);
                return parked;
            }
            answered_deferred = detach_deferred(app, conn); /* never NULL here: no batch tail is held yet */
        }

        if (can_coalesce(app, conn, served)) {
            /* queue it behind the batch instead of writing it: one write for the whole run (flush_batch,
             * or the next flush_connection, which puts the batch in front of its own response) */
            memcpy(app->batch_buf + app->batch_len, conn->out_buf, conn->out_len);
            app->batch_len += conn->out_len;
            conn->out_buf = NULL;
            conn->out_len = 0;
            conn->out_cap = 0;
            finish_request(app, conn);
            if (answered_deferred != NULL) {
                release_deferred(app, answered_deferred);
            }
            arena_reset(&app->arena);
            continue;
        }
        /* any batch queued so far goes out in front of this response (flush_connection: take_batch) */
        const int flushed = flush_connection(app, conn);
        if (answered_deferred != NULL) {
            release_deferred(app, answered_deferred); /* after the flush: its response bytes were in its arena */
        }
        /* conn may already be freed by flush_connection (a non-keep-alive response, or a
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
    /* the cap's last request is never coalesced (can_coalesce), so the batch is already empty here */
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
 * handle_readable is returning with conn still open. If in_buf is the borrowed App.read_buf,
 * either hand it back (nothing left to serve) or copy the unserved bytes - a partial request, or
 * requests pipelined behind a pending response - into an owned buffer of just that size (+ NUL, rounded up
 * to IN_BUF_GRANULE), compacted to offset 0 (request_len, chunk_scan and head_scan are relative to in_off,
 * so they stay valid). The leftover came out of a BUF_SIZE buffer, so the owned one is never larger. Returns 0, or -1 (conn closed: malloc failed, or a partial
 * request found the worker over ServerConfig.max_buffered_bytes - answered 503). Bytes pipelined behind
 * a pending response are copied even over budget: a response cannot be sent over the one in flight.
 */
static int stop_borrowing_read_buf(App *app, Connection *conn) {
    if (response_pending(conn) && conn->request_len > 0 && conn->in_off + conn->request_len >= conn->in_len) {
        /* the request behind the pending response has been dispatched (its views died with the
         * handler) and nothing is pipelined after it, so its bytes are dead. Drop them now rather than
         * hold a BUF_SIZE copy for the response's whole life - a res_stream subscriber can stay open
         * for hours. request_len = 0 makes the keep-alive reset treat the buffer as fully consumed. */
        release_in_buf(app, conn);
        conn->request_len = 0;
        return 0;
    }
    if (conn->in_buf != app->read_buf) {
        return 0;
    }
    const size_t leftover = conn->in_len - conn->in_off;
    if (leftover == 0) {
        release_in_buf(app, conn);
        return 0;
    }
    /* sized to what is left (+ NUL), rounded up to IN_BUF_GRANULE - never above BUF_SIZE, since the leftover
     * came out of a BUF_SIZE buffer. A slow client that has sent 20 bytes holds 512, not 8 KiB; a head that
     * fills it grows through grow_head_buf, a body through grow_in_buf. */
    const size_t owned_cap = (leftover + 1 + IN_BUF_GRANULE - 1) / IN_BUF_GRANULE * IN_BUF_GRANULE;
    if (!response_pending(conn) && !budget_allows(app, owned_cap)) {
        release_in_buf(app, conn); /* the partial request is dropped; App.read_buf is handed back first */
        reject_request(app, conn, 503);
        return -1;
    }
    char *owned = malloc(owned_cap);
    if (owned == NULL) {
        connection_close(app, conn); /* leaves the borrowed App.read_buf alone */
        return -1;
    }
    memcpy(owned, conn->in_buf + conn->in_off, leftover);
    owned[leftover] = '\0';
    conn->in_buf = owned;
    conn->in_cap = owned_cap;
    conn->in_len = leftover;
    conn->in_off = 0;
    sync_held_bytes(app, conn);
    return 0;
}

/*
 * read readiness while a producer stream is live (normally parked - see park_stream). Peeks one
 * byte without consuming it: EOF or a hard error means the peer is gone, so close now (freeing the
 * producer's ctx) instead of holding the stream until its next write fails. Real bytes are a pipelined
 * request that must wait for the stream to end: drop read interest so level-triggered readiness does
 * not spin; a disconnect is then noticed on the next write. Returns 0 (open) or -1 (closed).
 */
static int watch_stream_peer(App *app, Connection *conn) {
    char probe;
    const ssize_t n = recv(conn->fd, &probe, 1, MSG_PEEK);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        connection_close(app, conn);
        return -1;
    }
    if (n > 0 && (conn->events_watched & EVENT_READ)) {
        event_loop_unwatch_read(app, conn->fd);
    }
    return 0;
}

/* handle_readable's body. Returns 0 with conn still open, -1 once conn has been closed (freed). */
static int read_and_serve(App *app, Connection *conn) {
    if (response_pending(conn) || conn->in_off > 0) {
        /* a response is still draining (reads are normally unwatched meanwhile, see
         * wait_for_writable - this guards a readiness event already queued in the same batch), or
         * buffered pipelined requests are waiting for their handle_writable turn. Reading now would
         * append behind them; they are served first, in order. */
        if (response_pending(conn)) {
            return conn->stream_fn != NULL || conn->deferred != NULL ? watch_stream_peer(app, conn) : 0;
        }
        int served;
        const int status = serve_buffered_requests(app, conn, &served);
        if (status != SERVE_NEED_MORE) {
            return status == SERVE_CLOSED ? -1 : 0;
        }
    }

    if (conn->in_buf == NULL) {
        /* nothing buffered - read into the worker's shared buffer; stop_borrowing_read_buf
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
            /* First byte of a fresh request after an idle keep-alive gap: (re)start the deadline clock. */
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
        if (header_len == 0 && conn->in_cap >= BUF_SIZE) {
            reject_request(app, conn, 431);
            return -1;
        }
        const int grown = header_len == 0 ? grow_head_buf(app, conn)
                                          : grow_in_buf(app, conn, header_len, chunked, content_length);
        if (grown == 1) {
            return 0; /* wait for more read events */
        }
        reject_request(app, conn, grown == -1 ? 413 : grown == -2 ? 503 : 500);
        return -1;
    }
    return 0;
}

void handle_readable(App *app, Connection *conn) {
    if (read_and_serve(app, conn) == 0) {
        stop_borrowing_read_buf(app, conn); /* App.read_buf is never held across event-loop turns */
    }
}

void handle_writable(App *app, Connection *conn) {
    if (conn->deferred != NULL) {
        /* nothing is pending on the socket while deferred; write readiness only finishes a response whose
         * res_resume could not be queued */
        if (conn->deferred->state == DEFER_READY) {
            finish_deferred(app, conn->deferred);
        } else {
            event_loop_unwatch_write(app, conn->fd, conn);
        }
        return;
    }
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
         *: a pending response that hasn't accepted a single byte onto the socket in
         * WRITE_TIMEOUT_SECONDS is a client that stopped reading, not a slow one - close it rather
         * than hold the fd, arena and out_buf forever. Still-progressing writes (however slowly) are
         * left for flush_connection()/EVFILT_WRITE to keep draining. */
        if (conn->deferred != NULL) {
            /* waiting on the application, not on the client: only its own deadline applies */
            DeferredRequest *const dr = conn->deferred;
            if (dr->state == DEFER_WAITING && now >= dr->deadline) {
                const int is_head = dr->res->is_head_request;
                conn->keep_alive = 0;
                res_init(dr->res, conn); /* drop what the handler prepared for the real answer */
                dr->res->is_head_request = is_head;
                res_status(dr->res, 504);
                res_send(dr->res, status_text(504));
                dr->state = DEFER_READY;
                finish_deferred(app, dr);
            }
            continue;
        }
        if (response_pending(conn)) {
            if (conn->stream_paused) {
                /* a parked producer stream is idle by its own choice, not stalled: poll it again
                 * (at most once a second) instead of applying the write-stall deadline. */
                resume_stream(app, conn);
                continue;
            }
            if (now - conn->last_write_progress >= WRITE_TIMEOUT_SECONDS) {
                connection_close(app, conn); /* a response is already mid-flight: nothing left to say */
            }
            continue;
        }

        /* A request in flight is bounded by request_started, independent of how often a byte arrives -
         * closes the slow-drip case (one byte every N < 60s) that never goes "idle" under last_activity
         * alone. Headers incomplete: header deadline; headers already complete (body still
         * pending): the more generous body deadline. request_framing is cheap and this only runs once
         * per sweep second per connection, not on the per-byte hot path. */
        if (conn->request_started != 0) {
            size_t header_len = 0;
            int chunked = 0;
            if (conn->in_len > conn->in_off) {
                /* from in_off - bytes before it belong to a request already answered. */
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

void app_wake_streams(App *app) {
    if (app == NULL || app->connections == NULL) {
        return;
    }
    for (int fd = 0; fd < app->connections_cap; fd++) {
        Connection *conn = app->connections[fd];
        if (conn != NULL && conn->stream_fn != NULL && conn->stream_paused) {
            resume_stream(app, conn);
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

/* ---- application fds (app_watch_fd) ---- */

/* Makes the backend's interest in watched fd match what the application asked for. 0, or -1. The backends find
 * the tracked bits through App.watched (udata NULL), so an unchanged interest costs no syscall. */
static int apply_watch(App *app, const int fd) {
    const unsigned wanted = app->watched[fd].wanted;
    const int read_rc = (wanted & WATCH_READ) ? event_loop_watch_read(app, fd, NULL) : event_loop_unwatch_read(app, fd);
    const int write_rc = (wanted & WATCH_WRITE) ? event_loop_watch_write(app, fd, NULL)
                                                : event_loop_unwatch_write(app, fd, NULL);
    return read_rc == 0 && write_rc == 0 ? 0 : -1;
}

int app_watch_fd(App *app, int fd, unsigned events, FdReadyFn on_ready, void *udata) {
    if (app == NULL || fd < 0 || on_ready == NULL || (events & ~(WATCH_READ | WATCH_WRITE)) != 0 ||
        fd == app->server_fd || (fd < app->connections_cap && app->connections[fd] != NULL)) {
        return -1;
    }
    if (fd >= app->watched_cap) {
        int cap = app->watched_cap > 0 ? app->watched_cap : 16;
        while (cap <= fd) {
            cap *= 2;
        }
        WatchedFd *const grown = realloc(app->watched, (size_t)cap * sizeof(WatchedFd)); /* freed by app_destroy */
        if (grown == NULL) {
            return -1;
        }
        memset(grown + app->watched_cap, 0, (size_t)(cap - app->watched_cap) * sizeof(WatchedFd));
        app->watched = grown;
        app->watched_cap = cap;
    }
    WatchedFd *const w = &app->watched[fd];
    w->fn = on_ready;
    w->udata = udata;
    w->wanted = events;
    /* before the loop is open (an app_on_worker_start hook), app_listen_worker applies it once it is */
    return event_loop_is_open(app) ? apply_watch(app, fd) : 0;
}

int app_unwatch_fd(App *app, int fd) {
    if (app == NULL || fd < 0 || fd >= app->watched_cap || app->watched[fd].fn == NULL) {
        return -1;
    }
    int rc = 0;
    if (event_loop_is_open(app)) {
        app->watched[fd].wanted = 0;
        rc = apply_watch(app, fd); /* a real removal: the fd stays open, unlike event_loop_release_fd's case */
    }
    memset(&app->watched[fd], 0, sizeof(WatchedFd));
    return rc;
}

static void apply_pending_watches(App *app) {
    for (int fd = 0; fd < app->watched_cap; fd++) {
        if (app->watched[fd].fn != NULL && apply_watch(app, fd) != 0) {
            fprintf(stderr, "app_watch_fd: could not watch fd %d\n", fd);
        }
    }
}

/* ---- the event loop ---- */

/* Handles one polled event. Returns 1 when the process must stop serving now (second signal, drain deadline,
 * or a first signal with nothing left to drain), else 0. */
static int handle_event(App *app, const LoopEvent *ev) {
    if (ev->type == LOOP_EVENT_SIGNAL) {
        if (!app->is_shutting_down) {
            printf("\nReceived signal %d, draining connections...\n", ev->signo);
            app_stop(app);
            return app_count_connections(app) == 0;
        }
        fprintf(stderr, "\nReceived second signal %d, forcing shutdown\n", ev->signo);
        return 1;
    }
    if (ev->type == LOOP_EVENT_TIMER_IDLE) {
        close_idle_connections(app);
        return 0;
    }
    if (ev->type == LOOP_EVENT_TIMER_SHUTDOWN) {
        fprintf(stderr, "Shutdown timeout reached (%ds), force-closing remaining connections\n",
                SHUTDOWN_TIMEOUT_SECONDS);
        return 1;
    }
    if (ev->type == LOOP_EVENT_ACCEPT) {
#ifdef CEXPRESS_SINGLE_ACCEPTOR
        if (app->accept_via_fd_passing) {
            accept_passed_connections(app);
            return 0;
        }
#endif
        accept_connections(app);
        return 0;
    }
    const int fd = ev->fd;
    if (ev->type == LOOP_EVENT_EXTERNAL) {
        /* unwatched earlier in this batch (maybe by another callback): skip, never call a stale callback */
        if (fd >= 0 && fd < app->watched_cap && app->watched[fd].fn != NULL) {
            app->watched[fd].fn(app, fd, ev->ready, app->watched[fd].udata);
        }
        return 0;
    }
    if (fd < 0 || fd >= app->connections_cap || app->connections[fd] == NULL || app->connections[fd] != ev->conn) {
        /* Connection was closed earlier in this event batch (e.g. by app_stop or close_idle_connections) - skip
         * to avoid use-after-free. */
        return 0;
    }
    Connection *conn = ev->conn;
    if (ev->type == LOOP_EVENT_ERROR) {
        connection_close(app, conn);
    } else if (ev->type == LOOP_EVENT_READ) {
        handle_readable(app, conn);
    } else if (ev->type == LOOP_EVENT_WRITE) {
        handle_writable(app, conn);
    }
    return 0;
}

void app_on_turn_end(App *app, const TurnEndHook fn, void *const udata) {
    app->turn_end_hook = fn;
    app->turn_end_udata = fn != NULL ? udata : NULL;
}

int app_run_once(App *app, const int timeout_ms) {
    if (app->defer_ready_len > 0) {
        serve_resumed(app); /* resumed outside any event (before the loop started, or by code between turns) */
    }
    LoopEvent events[MAX_EVENTS];
    const int n = event_loop_poll(app, events, MAX_EVENTS, timeout_ms);
    if (n < 0) {
        return -1;
    }
    for (int i = 0; i < n; i++) {
        if (handle_event(app, &events[i])) {
            return LOOP_TURN_EXIT;
        }
        /* responses resumed by that event (an app_watch_fd callback, another connection's handler, a
         * producer) go out now, from the top level, before the next event */
        if (app->defer_ready_len > 0) {
            serve_resumed(app);
        }
    }
    if (app->turn_end_hook != NULL) {
        app->turn_end_hook(app, app->turn_end_udata);
    }
    if (app->is_shutting_down && app_count_connections(app) == 0) {
        printf("All connections drained. Server shutting down.\n");
        return LOOP_TURN_EXIT;
    }
    return n;
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

    /* CEXPRESS_SINGLE_ACCEPTOR only: a worker running under the single-acceptor cluster model
     * arrives here with server_fd already set to its control socket by
     * app_listen_worker_via_control_socket, and must not clobber it with a real listen socket of its
     * own - it never accept()s directly. Every other caller (standalone, or a Linux cluster worker)
     * is unaffected: accept_via_fd_passing is 0 for them, same bind-here behavior as before. */
    if (!app->accept_via_fd_passing) {
        app->server_fd = create_server_socket(app->config.bind_address, port);
        if (app->server_fd < 0) {
            fprintf(stderr, "app_listen_worker: could not create listening socket on %s port %d\n",
                    app->config.bind_address != NULL ? app->config.bind_address : "0.0.0.0", port);
            exit(EXIT_FAILURE);
        }
    }
    if (event_loop_init(app) != 0) {
        /* Not perror: errno may be stale by now. The backend that failed has already said why. */
        fprintf(stderr, "app_listen_worker: no usable event loop backend, exiting\n");
        exit(EXIT_FAILURE);
    }
    event_loop_watch_read(app, app->server_fd, NULL);
    apply_pending_watches(app); /* fds the worker-start hooks registered before the loop existed */

    if (!app->accept_via_fd_passing && (!cluster_is_worker() || cluster_worker_id() == 0)) {
        printf("Listening on port %d (%s)\n", port, event_loop_backend_name(app));
    }

    while (1) {
        const int n = app_run_once(app, -1);
        if (n == LOOP_TURN_EXIT) {
            break;
        }
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
    }

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

