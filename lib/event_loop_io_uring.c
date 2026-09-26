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
#include "event_loop_backend.h"

#define URING_ENTRIES 1024

/*
 * a poll's user_data is (generation << 32) | (fd + 1). 0 is reserved for completions nobody reads
 * (poll removals). The generation is App.poll_regs[fd].gen when the poll was armed, bumped on every arm,
 * so a completion carrying an older one belongs to a poll that was removed or replaced - most
 * importantly the kernel's -ECANCELED for a removed poll, which used to be reported as
 * LOOP_EVENT_ERROR and closed every keep-alive connection after one request.
 */
#define UDATA(fd, gen) (((__u64)(gen) << 32) | (__u64)((uint32_t)(fd) + 1))
#define UDATA_FD(udata) ((int)((uint32_t)(udata) - 1))
#define UDATA_GEN(udata) ((uint32_t)((udata) >> 32))

static void uring_close(App *app);

/* Grows App.poll_regs (doubling, new slots zeroed = nothing armed) until fd fits. NULL on OOM. */
static PollRegistration *registration_for(App *app, const int fd) {
    if (fd < 0) {
        return NULL;
    }
    if (fd >= app->poll_regs_cap) {
        int cap = app->poll_regs_cap > 0 ? app->poll_regs_cap : INITIAL_CONNECTION_TABLE_CAP;
        while (cap <= fd) {
            cap *= 2;
        }
        PollRegistration *grown = realloc(app->poll_regs, (size_t)cap * sizeof(PollRegistration));
        if (grown == NULL) {
            return NULL;
        }
        memset(grown + app->poll_regs_cap, 0, (size_t)(cap - app->poll_regs_cap) * sizeof(PollRegistration));
        app->poll_regs = grown;
        app->poll_regs_cap = cap;
    }
    return &app->poll_regs[fd];
}

/* An SQE, submitting what is queued first if the submission queue is full. */
static struct io_uring_sqe *next_sqe(struct io_uring *ring) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (sqe == NULL) {
        io_uring_submit(ring);
        sqe = io_uring_get_sqe(ring);
    }
    return sqe;
}

/* Queues a one-shot poll for fd's current interest under a new generation. */
static int arm_poll(struct io_uring *ring, const int fd, PollRegistration *reg) {
    struct io_uring_sqe *sqe = next_sqe(ring);
    if (sqe == NULL) {
        reg->armed = 0;
        return -1;
    }
    reg->gen++;
    io_uring_prep_poll_add(sqe, fd, reg->mask);
    io_uring_sqe_set_data64(sqe, UDATA(fd, reg->gen));
    reg->armed = 1;
    return 0;
}

/*
 * Makes `mask` (POLLIN/POLLOUT, 0 = nothing) fd's interest. An unchanged mask costs nothing (
 * flush_connection drops write interest after every keep-alive response, usually never set): the
 * one-shot poll in flight already covers it, and event_loop_poll re-arms after each event. Otherwise
 * the in-flight poll is removed (its completion then carries a stale generation) and a new one armed.
 * Only queues SQEs: event_loop_poll submits them in one io_uring_enter per loop turn.
 * Invariant this relies on: an fd is unwatched (mask 0) before it is closed - connection_close does
 * release_fd first - so a reused fd number never inherits a stale "already armed" mask.
 */
static int update_poll(App *app, const int fd, const uint32_t mask) {
    struct io_uring *ring = (struct io_uring *)app->ring;
    PollRegistration *reg = registration_for(app, fd);
    if (reg == NULL) {
        return -1;
    }
    if (reg->mask == mask) {
        return 0;
    }
    if (reg->armed) {
        struct io_uring_sqe *sqe = next_sqe(ring);
        if (sqe == NULL) {
            return -1;
        }
        io_uring_prep_poll_remove(sqe, UDATA(fd, reg->gen));
        io_uring_sqe_set_data64(sqe, 0); /* its own completion is ignored */
        reg->armed = 0;
        reg->gen++; /* whatever the removed poll still delivers is stale from here on */
    }
    reg->mask = (uint16_t)mask;
    return mask != 0 ? arm_poll(ring, fd, reg) : 0;
}

static int uring_init(App *app) {
    if (app == NULL) return -1;

    struct io_uring *ring = calloc(1, sizeof(struct io_uring));
    if (!ring) return -1;

    int ret = io_uring_queue_init(URING_ENTRIES, ring, 0);
    if (ret < 0) {
        /* the ring itself was refused (seccomp, io_uring_disabled, memlock, old kernel) and nothing
         * else was touched yet: tell the dispatcher, which logs errno as the reason. */
        free(ring);
        errno = -ret;
        return EVENT_LOOP_UNAVAILABLE;
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
        uring_close(app);
        return -1;
    }

    app->signal_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (app->signal_fd < 0) {
        sigprocmask(SIG_UNBLOCK, &mask, NULL);
        uring_close(app);
        return -1;
    }

    if (update_poll(app, app->signal_fd, POLLIN) != 0) {
        uring_close(app);
        return -1;
    }

    /* Idle timer via timerfd */
    app->timer_idle_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (app->timer_idle_fd < 0) {
        uring_close(app);
        return -1;
    }

    struct itimerspec its;
    its.it_interval.tv_sec = IDLE_SWEEP_INTERVAL_MS / 1000;
    its.it_interval.tv_nsec = (long)(IDLE_SWEEP_INTERVAL_MS % 1000) * 1000000L;
    its.it_value = its.it_interval;
    if (timerfd_settime(app->timer_idle_fd, 0, &its, NULL) < 0) {
        uring_close(app);
        return -1;
    }

    if (update_poll(app, app->timer_idle_fd, POLLIN) != 0) {
        uring_close(app);
        return -1;
    }
    io_uring_submit(ring);
    return 0;
}

static void uring_close(App *app) {
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
    free(app->poll_regs);
    app->poll_regs = NULL;
    app->poll_regs_cap = 0;
}

static int uring_is_open(const App *app) {
    return app != NULL && app->ring != NULL;
}

/* The tracked interest bits for fd: its Connection's (udata, or the connections table), else its app_watch_fd
 * entry's, else NULL (an fd nobody tracks: the listen socket, signalfd, timerfds). */
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

static int uring_watch_read(App *app, int fd, void *udata) {
    if (app == NULL || app->ring == NULL || fd < 0) return -1;
    int *const bits = interest_bits(app, fd, udata);

    uint32_t mask = POLLIN;
    if (bits != NULL) {
        if (*bits & EVENT_WRITE) mask |= POLLOUT;
        *bits |= EVENT_READ;
    }
    return update_poll(app, fd, mask);
}

static int uring_unwatch_read(App *app, int fd) {
    if (app == NULL || app->ring == NULL || fd < 0) return -1;
    int *const bits = interest_bits(app, fd, NULL);
    uint32_t mask = 0;
    if (bits != NULL) {
        *bits &= ~EVENT_READ;
        if (*bits & EVENT_WRITE) mask |= POLLOUT;
    }
    return update_poll(app, fd, mask);
}

static int uring_watch_write(App *app, int fd, void *udata) {
    if (app == NULL || app->ring == NULL || fd < 0) return -1;
    int *const bits = interest_bits(app, fd, udata);
    uint32_t mask = POLLOUT;
    if (bits != NULL) {
        if (*bits & EVENT_READ) mask |= POLLIN;
        *bits |= EVENT_WRITE;
    }
    return update_poll(app, fd, mask);
}

static int uring_unwatch_write(App *app, int fd, void *udata) {
    if (app == NULL || app->ring == NULL || fd < 0) return -1;
    int *const bits = interest_bits(app, fd, udata);
    uint32_t mask = 0;
    if (bits != NULL) {
        *bits &= ~EVENT_WRITE;
        if (*bits & EVENT_READ) mask |= POLLIN;
    }
    return update_poll(app, fd, mask);
}

static int uring_release_fd(App *app, int fd) {
    if (app == NULL || app->ring == NULL || fd < 0) return -1;
    int *const bits = interest_bits(app, fd, NULL);
    if (bits != NULL) {
        *bits = 0;
    }
    return update_poll(app, fd, 0);
}

static int uring_arm_shutdown_timer(App *app) {
    if (app == NULL || app->ring == NULL) return -1;
    if (app->timer_shutdown_fd < 0) {
        app->timer_shutdown_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (app->timer_shutdown_fd < 0) return -1;

        if (update_poll(app, app->timer_shutdown_fd, POLLIN) != 0) return -1;
    }
    struct itimerspec its;
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = 0;
    its.it_value.tv_sec = SHUTDOWN_TIMEOUT_SECONDS;
    its.it_value.tv_nsec = 0;
    if (timerfd_settime(app->timer_shutdown_fd, 0, &its, NULL) < 0) return -1;
    return 0;
}

static int uring_poll(App *app, LoopEvent *out_events, int max_events, int timeout_ms) {
    if (app == NULL || app->ring == NULL || out_events == NULL || max_events <= 0) return -1;

    struct io_uring *ring = (struct io_uring *)app->ring;
    struct io_uring_cqe *cqe;
    
    struct __kernel_timespec ts;
    ts.tv_sec = timeout_ms >= 0 ? timeout_ms / 1000 : 0;
    ts.tv_nsec = timeout_ms >= 0 ? (timeout_ms % 1000) * 1000000LL : 0;

    /* Interest changes and re-arms queued since the last poll: one submit for all of them, combined
     * with the wait when there is no timeout. */
    if (io_uring_sq_ready(ring) > 0) {
        if (timeout_ms < 0) {
            io_uring_submit_and_wait(ring, 1);
        } else {
            io_uring_submit(ring);
        }
    }

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
        
        const __u64 udata = io_uring_cqe_get_data64(cqe);
        if (udata == 0) {
            continue; /* a poll_remove's own completion */
        }
        const int fd = UDATA_FD(udata);
        int res = cqe->res;
        PollRegistration *reg = (fd >= 0 && fd < app->poll_regs_cap) ? &app->poll_regs[fd] : NULL;
        if (reg == NULL || !reg->armed || UDATA_GEN(udata) != reg->gen) {
            /* from a poll that has since been removed or replaced - its -ECANCELED farewell, or
             * readiness it reported just before. Not an event for whoever owns this fd now (a newer
             * interest, or a new connection on a reused fd number). */
            continue;
        }
        /* The live one-shot poll fired. Re-arm the same interest now (submitted with the next poll):
         * arming re-checks readiness, so a condition the handler leaves in place - unread bytes, a
         * still-writable socket - fires again next turn, the level-triggered behavior connection.c
         * relies on. Not after an error: the connection is about to be closed. */
        reg->armed = 0;
        if (res >= 0 && !(res & (POLLERR | POLLHUP | POLLNVAL))) {
            arm_poll(ring, fd, reg);
        }

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

            if (conn == NULL && fd < app->watched_cap && app->watched[fd].fn != NULL) {
                out_events[out_count].type = LOOP_EVENT_EXTERNAL;
                out_events[out_count].fd = fd;
                out_events[out_count].conn = NULL;
                out_events[out_count].signo = 0;
                out_events[out_count].ready = res < 0 ? WATCH_ERROR
                                                      : ((res & POLLIN) ? WATCH_READ : 0) | ((res & POLLOUT) ? WATCH_WRITE : 0) |
                                                            ((res & (POLLERR | POLLHUP | POLLNVAL)) ? WATCH_ERROR : 0);
                out_count++;
            } else if (res < 0) {
                out_events[out_count].type = LOOP_EVENT_ERROR;
                out_events[out_count].fd = fd;
                out_events[out_count].conn = conn;
                out_events[out_count].signo = 0;
                out_count++;
            } else {
                if (res & (POLLERR | POLLHUP | POLLNVAL)) {
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

/* the only exported symbol; event_loop_linux.c selects it at runtime (event_loop_backend.h). */
const EventLoopOps io_uring_loop_ops = {
    .name = "io_uring",
    .init = uring_init,
    .is_open = uring_is_open,
    .close_loop = uring_close,
    .watch_read = uring_watch_read,
    .unwatch_read = uring_unwatch_read,
    .watch_write = uring_watch_write,
    .unwatch_write = uring_unwatch_write,
    .release_fd = uring_release_fd,
    .arm_shutdown_timer = uring_arm_shutdown_timer,
    .poll_events = uring_poll,
};

#endif /* defined(__linux__) && !defined(CEXPRESS_USE_EPOLL) */
