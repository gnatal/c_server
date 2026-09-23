#if defined(__linux__) || defined(CEXPRESS_USE_EPOLL)

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/signalfd.h>
#include <time.h>
#include <sys/time.h>
#include "event_loop.h"
#include "event_loop_backend.h"

#ifndef __linux__
#ifndef _STRUCT_ITIMERSPEC
#define _STRUCT_ITIMERSPEC
struct itimerspec {
    struct timespec it_interval;
    struct timespec it_value;
};
#endif
#endif


static void epoll_close(App *app);

static int epoll_init(App *app) {
    if (app == NULL) {
        return -1;
    }

    app->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (app->epoll_fd < 0) {
        perror("epoll_create1");
        return -1;
    }

    app->timer_idle_fd = -1;
    app->timer_shutdown_fd = -1;
    app->signal_fd = -1;

    /*
     * Synchronous signal handling via signalfd(2): block SIGINT and SIGTERM so
     * standard asynchronous delivery does not terminate the process, and register
     * a signalfd with epoll to receive notifications synchronously in the event loop.
     */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
        perror("sigprocmask: SIG_BLOCK");
        close(app->epoll_fd);
        app->epoll_fd = -1;
        return -1;
    }

    app->signal_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (app->signal_fd < 0) {
        perror("signalfd");
        sigprocmask(SIG_UNBLOCK, &mask, NULL);
        close(app->epoll_fd);
        app->epoll_fd = -1;
        return -1;
    }

    struct epoll_event sig_ev;
    memset(&sig_ev, 0, sizeof(sig_ev));
    sig_ev.events = EPOLLIN;
    sig_ev.data.fd = app->signal_fd;
    if (epoll_ctl(app->epoll_fd, EPOLL_CTL_ADD, app->signal_fd, &sig_ev) < 0) {
        perror("epoll_ctl: signalfd");
        close(app->signal_fd);
        app->signal_fd = -1;
        sigprocmask(SIG_UNBLOCK, &mask, NULL);
        close(app->epoll_fd);
        app->epoll_fd = -1;
        return -1;
    }

    /*
     * Periodic idle-connection sweep timer via timerfd(2):
     * Configured to trigger every IDLE_SWEEP_INTERVAL_MS.
     */
    app->timer_idle_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (app->timer_idle_fd < 0) {
        perror("timerfd_create: idle timer");
        epoll_close(app);
        return -1;
    }

    struct itimerspec its;
    its.it_interval.tv_sec = IDLE_SWEEP_INTERVAL_MS / 1000;
    its.it_interval.tv_nsec = (long)(IDLE_SWEEP_INTERVAL_MS % 1000) * 1000000L;
    its.it_value = its.it_interval;
    if (timerfd_settime(app->timer_idle_fd, 0, &its, NULL) < 0) {
        perror("timerfd_settime: idle timer");
        epoll_close(app);
        return -1;
    }

    struct epoll_event timer_ev;
    memset(&timer_ev, 0, sizeof(timer_ev));
    timer_ev.events = EPOLLIN;
    timer_ev.data.fd = app->timer_idle_fd;
    if (epoll_ctl(app->epoll_fd, EPOLL_CTL_ADD, app->timer_idle_fd, &timer_ev) < 0) {
        perror("epoll_ctl: idle timerfd");
        epoll_close(app);
        return -1;
    }

    return 0;
}

static int epoll_is_open(const App *app) {
    return app != NULL && app->epoll_fd >= 0;
}

static void epoll_close(App *app) {
    if (app == NULL) {
        return;
    }

    /* Unblock signals and restore default dispositions */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_UNBLOCK, &mask, NULL);
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);

    if (app->signal_fd >= 0) {
        close(app->signal_fd);
        app->signal_fd = -1;
    }
    if (app->timer_idle_fd >= 0) {
        close(app->timer_idle_fd);
        app->timer_idle_fd = -1;
    }
    if (app->timer_shutdown_fd >= 0) {
        close(app->timer_shutdown_fd);
        app->timer_shutdown_fd = -1;
    }
    if (app->epoll_fd >= 0) {
        close(app->epoll_fd);
        app->epoll_fd = -1;
    }
}

static int epoll_watch_read(App *app, int fd, void *udata) {
    if (app == NULL || app->epoll_fd < 0 || fd < 0) {
        return -1;
    }

    Connection *conn = (Connection *)udata;
    if (conn == NULL && fd < app->connections_cap && app->connections != NULL) {
        conn = app->connections[fd];
    }

    int op = EPOLL_CTL_ADD;
    uint32_t events = EPOLLIN;
    if (conn != NULL) {
        if (conn->events_watched & EVENT_WRITE) {
            events |= EPOLLOUT;
        }
        if (conn->events_watched != 0) {
            op = EPOLL_CTL_MOD;
        }
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.fd = fd;

    if (epoll_ctl(app->epoll_fd, op, fd, &ev) < 0) {
        if (errno == EEXIST && op == EPOLL_CTL_ADD) {
            if (epoll_ctl(app->epoll_fd, EPOLL_CTL_MOD, fd, &ev) < 0) {
                return -1;
            }
        } else {
            return -1;
        }
    }

    if (conn != NULL) {
        conn->events_watched |= EVENT_READ;
    }
    return 0;
}

static int epoll_unwatch_read(App *app, int fd) {
    if (app == NULL || app->epoll_fd < 0 || fd < 0) {
        return -1;
    }

    Connection *conn = (fd < app->connections_cap && app->connections != NULL) ? app->connections[fd] : NULL;
    if (conn != NULL) {
        conn->events_watched &= ~EVENT_READ;
        if (conn->events_watched == 0) {
            return epoll_ctl(app->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
        } else {
            struct epoll_event ev;
            memset(&ev, 0, sizeof(ev));
            ev.events = EPOLLOUT;
            ev.data.fd = fd;
            return epoll_ctl(app->epoll_fd, EPOLL_CTL_MOD, fd, &ev);
        }
    }

    return epoll_ctl(app->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
}

static int epoll_watch_write(App *app, int fd, void *udata) {
    if (app == NULL || app->epoll_fd < 0 || fd < 0) {
        return -1;
    }

    Connection *conn = (Connection *)udata;
    if (conn == NULL && fd < app->connections_cap && app->connections != NULL) {
        conn = app->connections[fd];
    }

    int op = EPOLL_CTL_ADD;
    uint32_t events = EPOLLOUT;
    if (conn != NULL) {
        if (conn->events_watched & EVENT_READ) {
            events |= EPOLLIN;
        }
        if (conn->events_watched != 0) {
            op = EPOLL_CTL_MOD;
        }
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.fd = fd;

    if (epoll_ctl(app->epoll_fd, op, fd, &ev) < 0) {
        if (errno == EEXIST && op == EPOLL_CTL_ADD) {
            if (epoll_ctl(app->epoll_fd, EPOLL_CTL_MOD, fd, &ev) < 0) {
                return -1;
            }
        } else {
            return -1;
        }
    }

    if (conn != NULL) {
        conn->events_watched |= EVENT_WRITE;
    }
    return 0;
}

static int epoll_unwatch_write(App *app, int fd, void *udata) {
    if (app == NULL || app->epoll_fd < 0 || fd < 0) {
        return -1;
    }

    Connection *conn = (Connection *)udata;
    if (conn == NULL && fd < app->connections_cap && app->connections != NULL) {
        conn = app->connections[fd];
    }

    if (conn != NULL) {
        conn->events_watched &= ~EVENT_WRITE;
        if (conn->events_watched == 0) {
            return epoll_ctl(app->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
        } else {
            struct epoll_event ev;
            memset(&ev, 0, sizeof(ev));
            ev.events = EPOLLIN;
            ev.data.fd = fd;
            return epoll_ctl(app->epoll_fd, EPOLL_CTL_MOD, fd, &ev);
        }
    }

    return epoll_ctl(app->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
}

static int epoll_unwatch_all(App *app, int fd) {
    if (app == NULL || app->epoll_fd < 0 || fd < 0) {
        return -1;
    }

    if (fd < app->connections_cap && app->connections != NULL && app->connections[fd] != NULL) {
        app->connections[fd]->events_watched = 0;
    }

    int rc = epoll_ctl(app->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
    if (rc < 0 && (errno == ENOENT || errno == EBADF)) {
        return 0;
    }
    return rc;
}

static int epoll_arm_shutdown_timer(App *app) {
    if (app == NULL || app->epoll_fd < 0) {
        return -1;
    }

    if (app->timer_shutdown_fd < 0) {
        app->timer_shutdown_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (app->timer_shutdown_fd < 0) {
            perror("timerfd_create: shutdown timer");
            return -1;
        }

        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN;
        ev.data.fd = app->timer_shutdown_fd;
        if (epoll_ctl(app->epoll_fd, EPOLL_CTL_ADD, app->timer_shutdown_fd, &ev) < 0) {
            perror("epoll_ctl: shutdown timerfd");
            close(app->timer_shutdown_fd);
            app->timer_shutdown_fd = -1;
            return -1;
        }
    }

    struct itimerspec its;
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = 0;
    its.it_value.tv_sec = SHUTDOWN_TIMEOUT_SECONDS;
    its.it_value.tv_nsec = 0;
    if (timerfd_settime(app->timer_shutdown_fd, 0, &its, NULL) < 0) {
        perror("timerfd_settime: shutdown timer");
        return -1;
    }

    return 0;
}

static int epoll_poll(App *app, LoopEvent *out_events, int max_events, int timeout_ms) {
    if (app == NULL || app->epoll_fd < 0 || out_events == NULL || max_events <= 0) {
        return -1;
    }

    const int batch_cap = max_events > MAX_EVENTS ? MAX_EVENTS : max_events;
    struct epoll_event ep_events[MAX_EVENTS];
    int n = epoll_wait(app->epoll_fd, ep_events, batch_cap, timeout_ms);
    if (n <= 0) {
        return n;
    }

    int out_count = 0;
    for (int i = 0; i < n && out_count < max_events; i++) {
        int fd = ep_events[i].data.fd;

        if (app->signal_fd >= 0 && fd == app->signal_fd) {
            struct signalfd_siginfo fdsi;
            ssize_t s = read(app->signal_fd, &fdsi, sizeof(fdsi));
            if (s == (ssize_t)sizeof(fdsi)) {
                out_events[out_count].type = LOOP_EVENT_SIGNAL;
                out_events[out_count].fd = fd;
                out_events[out_count].conn = NULL;
                out_events[out_count].signo = (int)fdsi.ssi_signo;
                out_count++;
            }
        } else if (app->timer_idle_fd >= 0 && fd == app->timer_idle_fd) {
            uint64_t expirations = 0;
            ssize_t s = read(app->timer_idle_fd, &expirations, sizeof(expirations));
            (void)s;
            out_events[out_count].type = LOOP_EVENT_TIMER_IDLE;
            out_events[out_count].fd = fd;
            out_events[out_count].conn = NULL;
            out_events[out_count].signo = 0;
            out_count++;
        } else if (app->timer_shutdown_fd >= 0 && fd == app->timer_shutdown_fd) {
            uint64_t expirations = 0;
            ssize_t s = read(app->timer_shutdown_fd, &expirations, sizeof(expirations));
            (void)s;
            out_events[out_count].type = LOOP_EVENT_TIMER_SHUTDOWN;
            out_events[out_count].fd = fd;
            out_events[out_count].conn = NULL;
            out_events[out_count].signo = 0;
            out_count++;
        } else if (app->server_fd >= 0 && fd == app->server_fd) {
            out_events[out_count].type = LOOP_EVENT_ACCEPT;
            out_events[out_count].fd = fd;
            out_events[out_count].conn = NULL;
            out_events[out_count].signo = 0;
            out_count++;
        } else {
            Connection *conn = (fd >= 0 && fd < app->connections_cap && app->connections != NULL)
                                   ? app->connections[fd]
                                   : NULL;

            if (ep_events[i].events & (EPOLLERR | EPOLLHUP)) {
                out_events[out_count].type = LOOP_EVENT_ERROR;
                out_events[out_count].fd = fd;
                out_events[out_count].conn = conn;
                out_events[out_count].signo = 0;
                out_count++;
            } else {
                if (ep_events[i].events & EPOLLIN) {
                    out_events[out_count].type = LOOP_EVENT_READ;
                    out_events[out_count].fd = fd;
                    out_events[out_count].conn = conn;
                    out_events[out_count].signo = 0;
                    out_count++;
                }
                if ((ep_events[i].events & EPOLLOUT) && out_count < max_events) {
                    out_events[out_count].type = LOOP_EVENT_WRITE;
                    out_events[out_count].fd = fd;
                    out_events[out_count].conn = conn;
                    out_events[out_count].signo = 0;
                    out_count++;
                }
            }
        }
    }

    return out_count;
}

/* C5: the only exported symbol; event_loop_linux.c selects it at runtime (event_loop_backend.h). */
const EventLoopOps epoll_loop_ops = {
    .name = "epoll",
    .init = epoll_init,
    .is_open = epoll_is_open,
    .close_loop = epoll_close,
    .watch_read = epoll_watch_read,
    .unwatch_read = epoll_unwatch_read,
    .watch_write = epoll_watch_write,
    .unwatch_write = epoll_unwatch_write,
    .unwatch_all = epoll_unwatch_all,
    .arm_shutdown_timer = epoll_arm_shutdown_timer,
    .poll_events = epoll_poll,
};

#endif /* defined(__linux__) || defined(CEXPRESS_USE_EPOLL) */
