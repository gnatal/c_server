#if !defined(__linux__) || defined(CEXPRESS_USE_KQUEUE)

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/event.h>
#include <sys/time.h>
#include "event_loop.h"

int event_loop_init(App *app) {
    if (app == NULL) {
        return -1;
    }

    app->kq = kqueue();
    if (app->kq < 0) {
        perror("kqueue");
        return -1;
    }

    /*
     * Intercept SIGINT and SIGTERM for graceful shutdown. Ignore their default
     * dispositions (preventing asynchronous termination) and monitor them
     * synchronously via kqueue's EVFILT_SIGNAL.
     */
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);

    struct kevent sig_changes[2];
    EV_SET(&sig_changes[0], SIGINT, EVFILT_SIGNAL, EV_ADD | EV_ENABLE, 0, 0, NULL);
    EV_SET(&sig_changes[1], SIGTERM, EVFILT_SIGNAL, EV_ADD | EV_ENABLE, 0, 0, NULL);
    if (kevent(app->kq, sig_changes, 2, NULL, 0, NULL) < 0) {
        perror("kevent: EVFILT_SIGNAL");
        close(app->kq);
        app->kq = -1;
        return -1;
    }

    /*
     * Periodic timer that drives close_idle_connections(): ident 1 is arbitrary
     * and lives in the EVFILT_TIMER namespace, separate from socket fds.
     */
    struct kevent timer_change;
    EV_SET(&timer_change, 1, EVFILT_TIMER, EV_ADD | EV_ENABLE, 0, IDLE_SWEEP_INTERVAL_MS, NULL);
    if (kevent(app->kq, &timer_change, 1, NULL, 0, NULL) < 0) {
        perror("kevent: EVFILT_TIMER");
        close(app->kq);
        app->kq = -1;
        return -1;
    }

    return 0;
}

const char *event_loop_backend_name(const App *app) {
    return event_loop_is_open(app) ? "kqueue" : "none";
}

int event_loop_is_open(const App *app) {
    return app != NULL && app->kq >= 0;
}

void event_loop_close(App *app) {
    if (app == NULL) {
        return;
    }

    /* Restore default signal dispositions */
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);

    if (app->kq >= 0) {
        close(app->kq);
        app->kq = -1;
    }
}

/* The tracked interest bits for fd: its Connection's, or its app_watch_fd entry's, or NULL (listen
 * socket). Tracking events_watched lets watch/unwatch skip the kevent() syscall when the kernel already
 * has the requested state: every keep-alive response used to cost one failing EV_DELETE (ENOENT) for a
 * write filter never added. */
static int *interest_bits(const App *app, const int fd, void *udata) {
    if (udata != NULL) {
        return &((Connection *)udata)->events_watched;
    }
    if (app->connections != NULL && fd < app->connections_cap && app->connections[fd] != NULL) {
        return &app->connections[fd]->events_watched;
    }
    if (fd < app->watched_cap && app->watched[fd].fn != NULL) {
        return &app->watched[fd].events_watched;
    }
    return NULL;
}

static int kevent_change(App *app, const int fd, const int16_t filter, const uint16_t flags, void *udata) {
    struct kevent change;
    EV_SET(&change, fd, filter, flags, 0, 0, udata);
    return kevent(app->kq, &change, 1, NULL, 0, NULL);
}

static int watch(App *app, const int fd, const int16_t filter, const int bit, void *udata) {
    if (app == NULL || app->kq < 0 || fd < 0) {
        return -1;
    }
    int *const bits = interest_bits(app, fd, udata);
    if (bits != NULL && (*bits & bit)) {
        return 0;
    }
    const int rc = kevent_change(app, fd, filter, EV_ADD | EV_ENABLE, udata);
    if (rc == 0 && bits != NULL) {
        *bits |= bit;
    }
    return rc;
}

static int unwatch(App *app, const int fd, const int16_t filter, const int bit) {
    if (app == NULL || app->kq < 0 || fd < 0) {
        return -1;
    }
    int *const bits = interest_bits(app, fd, NULL);
    if (bits != NULL && !(*bits & bit)) {
        return 0;
    }
    const int rc = kevent_change(app, fd, filter, EV_DELETE, NULL);
    if (bits != NULL) {
        *bits &= ~bit;
    }
    if (rc < 0 && (errno == ENOENT || errno == EBADF)) {
        return 0;
    }
    return rc;
}

int event_loop_watch_read(App *app, int fd, void *udata) {
    return watch(app, fd, EVFILT_READ, EVENT_READ, udata);
}

int event_loop_unwatch_read(App *app, int fd) {
    return unwatch(app, fd, EVFILT_READ, EVENT_READ);
}

int event_loop_watch_write(App *app, int fd, void *udata) {
    return watch(app, fd, EVFILT_WRITE, EVENT_WRITE, udata);
}

int event_loop_unwatch_write(App *app, int fd, void *udata) {
    (void)udata;
    return unwatch(app, fd, EVFILT_WRITE, EVENT_WRITE);
}

int event_loop_release_fd(App *app, int fd) {
    if (app == NULL || app->kq < 0 || fd < 0) {
        return -1;
    }
    int *const bits = interest_bits(app, fd, NULL);
    if (bits != NULL) {
        *bits = 0;
    }
    /* No EV_DELETE: the caller closes fd next, and close() drops every knote on it. */
    return 0;
}

int event_loop_arm_shutdown_timer(App *app) {
    if (app == NULL || app->kq < 0) {
        return -1;
    }
    struct kevent shutdown_timer;
    EV_SET(&shutdown_timer, 2, EVFILT_TIMER, EV_ADD | EV_ENABLE | EV_ONESHOT, 0,
           SHUTDOWN_TIMEOUT_SECONDS * 1000, NULL);
    return kevent(app->kq, &shutdown_timer, 1, NULL, 0, NULL);
}

int event_loop_poll(App *app, LoopEvent *out_events, int max_events, int timeout_ms) {
    if (app == NULL || app->kq < 0 || out_events == NULL || max_events <= 0) {
        return -1;
    }

    struct timespec ts;
    struct timespec *pts = NULL;
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        pts = &ts;
    }

    const int batch_cap = max_events > MAX_EVENTS ? MAX_EVENTS : max_events;
    struct kevent kevs[MAX_EVENTS];
    int n = kevent(app->kq, NULL, 0, kevs, batch_cap, pts);
    if (n <= 0) {
        return n;
    }

    for (int i = 0; i < n; i++) {
        const struct kevent *kev = &kevs[i];
        LoopEvent *ev = &out_events[i];
        ev->fd = (int)kev->ident;
        ev->conn = (Connection *)kev->udata;
        ev->signo = 0;
        ev->ready = 0;

        if (kev->filter == EVFILT_SIGNAL) {
            ev->type = LOOP_EVENT_SIGNAL;
            ev->signo = (int)kev->ident;
        } else if (kev->filter == EVFILT_TIMER) {
            if (kev->ident == 1) {
                ev->type = LOOP_EVENT_TIMER_IDLE;
            } else if (kev->ident == 2) {
                ev->type = LOOP_EVENT_TIMER_SHUTDOWN;
            } else {
                ev->type = LOOP_EVENT_NONE;
            }
        } else if (app->server_fd >= 0 && (int)kev->ident == app->server_fd) {
            ev->type = LOOP_EVENT_ACCEPT;
        } else if (kev->udata == NULL && ev->fd < app->watched_cap && app->watched[ev->fd].fn != NULL) {
            /* app_watch_fd registers with NULL udata; a connection always carries its Connection */
            ev->type = LOOP_EVENT_EXTERNAL;
            ev->ready = kev->filter == EVFILT_WRITE ? WATCH_WRITE : WATCH_READ;
            if (kev->flags & (EV_ERROR | EV_EOF)) {
                ev->ready |= WATCH_ERROR;
            }
        } else {
            if (kev->flags & EV_ERROR) {
                ev->type = LOOP_EVENT_ERROR;
            } else if (kev->filter == EVFILT_READ) {
                ev->type = LOOP_EVENT_READ;
            } else if (kev->filter == EVFILT_WRITE) {
                ev->type = LOOP_EVENT_WRITE;
            } else {
                ev->type = LOOP_EVENT_NONE;
            }
        }
    }

    return n;
}

#endif /* !defined(__linux__) || defined(CEXPRESS_USE_KQUEUE) */
