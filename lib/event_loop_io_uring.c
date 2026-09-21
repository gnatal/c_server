#if defined(__linux__) && !defined(CEXPRESS_USE_EPOLL)

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <poll.h>
#include <sys/timerfd.h>
#include <sys/signalfd.h>
#include <time.h>
#include <sys/time.h>
#include <liburing.h>
#include "event_loop.h"

#define URING_ENTRIES 1024

/* Encode fd into user_data for quick retrieval. */
#define UDATA_FD(fd) ((__u64)(fd) + 1)

int event_loop_init(App *app) {
    if (app == NULL) return -1;

    struct io_uring *ring = calloc(1, sizeof(struct io_uring));
    if (!ring) return -1;

    int ret = io_uring_queue_init(URING_ENTRIES, ring, 0);
    if (ret < 0) {
        fprintf(stderr, "io_uring_queue_init failed: %d (%s)\n", ret, strerror(-ret));
        free(ring);
        return -1;
    }
    app->ring = ring;

    app->timer_idle_fd = -1;
    app->timer_shutdown_fd = -1;
    app->signal_fd = -1;

    /* Signal handling via signalfd */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
        event_loop_close(app);
        return -1;
    }

    app->signal_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (app->signal_fd < 0) {
        sigprocmask(SIG_UNBLOCK, &mask, NULL);
        event_loop_close(app);
        return -1;
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    io_uring_prep_poll_multishot(sqe, app->signal_fd, POLLIN);
    io_uring_sqe_set_data64(sqe, UDATA_FD(app->signal_fd));

    /* Idle timer via timerfd */
    app->timer_idle_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (app->timer_idle_fd < 0) {
        event_loop_close(app);
        return -1;
    }

    struct itimerspec its;
    its.it_interval.tv_sec = IDLE_SWEEP_INTERVAL_MS / 1000;
    its.it_interval.tv_nsec = (long)(IDLE_SWEEP_INTERVAL_MS % 1000) * 1000000L;
    its.it_value = its.it_interval;
    if (timerfd_settime(app->timer_idle_fd, 0, &its, NULL) < 0) {
        event_loop_close(app);
        return -1;
    }

    sqe = io_uring_get_sqe(ring);
    io_uring_prep_poll_multishot(sqe, app->timer_idle_fd, POLLIN);
    io_uring_sqe_set_data64(sqe, UDATA_FD(app->timer_idle_fd));

    io_uring_submit(ring);
    return 0;
}

void event_loop_close(App *app) {
    if (app == NULL || app->ring == NULL) return;

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_UNBLOCK, &mask, NULL);
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);

    if (app->signal_fd >= 0) { close(app->signal_fd); app->signal_fd = -1; }
    if (app->timer_idle_fd >= 0) { close(app->timer_idle_fd); app->timer_idle_fd = -1; }
    if (app->timer_shutdown_fd >= 0) { close(app->timer_shutdown_fd); app->timer_shutdown_fd = -1; }

    struct io_uring *ring = (struct io_uring *)app->ring;
    io_uring_queue_exit(ring);
    free(ring);
    app->ring = NULL;
}

/* Helper to update poll mask */
static int update_poll(App *app, int fd, uint32_t mask) {
    struct io_uring *ring = (struct io_uring *)app->ring;
    struct io_uring_sqe *sqe;

    /* Issue a remove to cancel the existing multishot poll, then add a new one.
     * IORING_POLL_UPDATE exists, but remove/add is universally compatible with 5.13+. */
    sqe = io_uring_get_sqe(ring);
    if (!sqe) {
        io_uring_submit(ring);
        sqe = io_uring_get_sqe(ring);
        if (!sqe) return -1;
    }
    /* We match the previous request by user_data */
    io_uring_prep_poll_remove(sqe, UDATA_FD(fd));
    io_uring_sqe_set_data64(sqe, 0); /* Ignore the remove completion */

    if (mask != 0) {
        sqe = io_uring_get_sqe(ring);
        if (!sqe) {
            io_uring_submit(ring);
            sqe = io_uring_get_sqe(ring);
            if (!sqe) return -1;
        }
        io_uring_prep_poll_multishot(sqe, fd, mask);
        io_uring_sqe_set_data64(sqe, UDATA_FD(fd));
    }

    io_uring_submit(ring);
    return 0;
}

int event_loop_watch_read(App *app, int fd, void *udata) {
    if (app == NULL || app->ring == NULL || fd < 0) return -1;
    Connection *conn = (Connection *)udata;
    if (conn == NULL && fd < app->connections_cap && app->connections != NULL) {
        conn = app->connections[fd];
    }

    uint32_t mask = POLLIN;
    if (conn != NULL) {
        if (conn->events_watched & EVENT_WRITE) mask |= POLLOUT;
        conn->events_watched |= EVENT_READ;
    }
    return update_poll(app, fd, mask);
}

int event_loop_unwatch_read(App *app, int fd) {
    if (app == NULL || app->ring == NULL || fd < 0) return -1;
    Connection *conn = (fd < app->connections_cap && app->connections != NULL) ? app->connections[fd] : NULL;
    uint32_t mask = 0;
    if (conn != NULL) {
        conn->events_watched &= ~EVENT_READ;
        if (conn->events_watched & EVENT_WRITE) mask |= POLLOUT;
    }
    return update_poll(app, fd, mask);
}

int event_loop_watch_write(App *app, int fd, void *udata) {
    if (app == NULL || app->ring == NULL || fd < 0) return -1;
    Connection *conn = (Connection *)udata;
    if (conn == NULL && fd < app->connections_cap && app->connections != NULL) {
        conn = app->connections[fd];
    }
    uint32_t mask = POLLOUT;
    if (conn != NULL) {
        if (conn->events_watched & EVENT_READ) mask |= POLLIN;
        conn->events_watched |= EVENT_WRITE;
    }
    return update_poll(app, fd, mask);
}

int event_loop_unwatch_write(App *app, int fd, void *udata) {
    if (app == NULL || app->ring == NULL || fd < 0) return -1;
    Connection *conn = (Connection *)udata;
    if (conn == NULL && fd < app->connections_cap && app->connections != NULL) {
        conn = app->connections[fd];
    }
    uint32_t mask = 0;
    if (conn != NULL) {
        conn->events_watched &= ~EVENT_WRITE;
        if (conn->events_watched & EVENT_READ) mask |= POLLIN;
    }
    return update_poll(app, fd, mask);
}

int event_loop_unwatch_all(App *app, int fd) {
    if (app == NULL || app->ring == NULL || fd < 0) return -1;
    if (fd < app->connections_cap && app->connections != NULL && app->connections[fd] != NULL) {
        app->connections[fd]->events_watched = 0;
    }
    return update_poll(app, fd, 0);
}

int event_loop_arm_shutdown_timer(App *app) {
    if (app == NULL || app->ring == NULL) return -1;
    if (app->timer_shutdown_fd < 0) {
        app->timer_shutdown_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (app->timer_shutdown_fd < 0) return -1;

        struct io_uring *ring = (struct io_uring *)app->ring;
        struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
        if (!sqe) return -1;
        io_uring_prep_poll_multishot(sqe, app->timer_shutdown_fd, POLLIN);
        io_uring_sqe_set_data64(sqe, UDATA_FD(app->timer_shutdown_fd));
        io_uring_submit(ring);
    }
    struct itimerspec its;
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = 0;
    its.it_value.tv_sec = SHUTDOWN_TIMEOUT_SECONDS;
    its.it_value.tv_nsec = 0;
    if (timerfd_settime(app->timer_shutdown_fd, 0, &its, NULL) < 0) return -1;
    return 0;
}

int event_loop_poll(App *app, LoopEvent *out_events, int max_events, int timeout_ms) {
    if (app == NULL || app->ring == NULL || out_events == NULL || max_events <= 0) return -1;

    struct io_uring *ring = (struct io_uring *)app->ring;
    struct io_uring_cqe *cqe;
    
    struct __kernel_timespec ts;
    ts.tv_sec = timeout_ms >= 0 ? timeout_ms / 1000 : 0;
    ts.tv_nsec = timeout_ms >= 0 ? (timeout_ms % 1000) * 1000000LL : 0;

    int ret;
    if (timeout_ms >= 0) {
        ret = io_uring_wait_cqe_timeout(ring, &cqe, &ts);
    } else {
        ret = io_uring_wait_cqe(ring, &cqe);
    }

    if (ret < 0) {
        if (ret == -ETIME) return 0;
        if (ret == -EINTR) { errno = EINTR; return -1; }
        errno = -ret;
        return -1;
    }

    int out_count = 0;
    unsigned head;
    unsigned int advanced = 0;
    
    io_uring_for_each_cqe(ring, head, cqe) {
        if (out_count >= max_events) break;
        advanced++;
        
        __u64 udata = io_uring_cqe_get_data64(cqe);
        if (udata == 0) {
            /* This was a poll_remove completion or ignored request. */
            continue;
        }
        int fd = (int)(udata - 1);
        int res = cqe->res;

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
                                   ? app->connections[fd] : NULL;

            if (res < 0) {
                out_events[out_count].type = LOOP_EVENT_ERROR;
                out_events[out_count].fd = fd;
                out_events[out_count].conn = conn;
                out_events[out_count].signo = 0;
                out_count++;
            } else {
                if (res & (POLLERR | POLLHUP)) {
                    out_events[out_count].type = LOOP_EVENT_ERROR;
                    out_events[out_count].fd = fd;
                    out_events[out_count].conn = conn;
                    out_events[out_count].signo = 0;
                    out_count++;
                } else {
                    if (res & POLLIN) {
                        out_events[out_count].type = LOOP_EVENT_READ;
                        out_events[out_count].fd = fd;
                        out_events[out_count].conn = conn;
                        out_events[out_count].signo = 0;
                        out_count++;
                    }
                    if ((res & POLLOUT) && out_count < max_events) {
                        out_events[out_count].type = LOOP_EVENT_WRITE;
                        out_events[out_count].fd = fd;
                        out_events[out_count].conn = conn;
                        out_events[out_count].signo = 0;
                        out_count++;
                    }
                }
            }
        }
    }

    if (advanced > 0) {
        io_uring_cq_advance(ring, advanced);
    }

    return out_count;
}

#endif /* defined(__linux__) && !defined(CEXPRESS_USE_EPOLL) */
