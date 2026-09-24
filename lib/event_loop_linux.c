#if defined(__linux__) || defined(CEXPRESS_USE_EPOLL)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "event_loop.h"
#include "event_loop_backend.h"

/*
 * runtime backend selection. epoll is the default: io_uring here is only a readiness poller (one SQE and
 * one CQE per event on top of the same recv/write), MEASURED 20-25% slower than epoll (lib/CLAUDE.md,
 * "Backend speed on Linux"). CEXPRESS_EVENT_LOOP=io_uring opts in, with no fallback: if the kernel or the
 * container runtime refuses the ring, init fails. CEXPRESS_EVENT_LOOP=epoll names the default; an empty
 * value means unset; any other value is refused. Built with CEXPRESS_USE_EPOLL (Linux NO_URING=1, or macOS
 * epoll-shim), epoll is the only one.
 * Every other function forwards through App.loop_ops, set only while a loop is open: one indirect call,
 * the same one per call site, so the branch predictor pins it.
 */
int event_loop_init(App *app) {
    if (app == NULL) {
        return -1;
    }
    const char *forced = getenv("CEXPRESS_EVENT_LOOP");
    if (forced != NULL && forced[0] == '\0') {
        forced = NULL;
    }
    if (forced != NULL && strcmp(forced, "epoll") != 0 && strcmp(forced, "io_uring") != 0) {
        fprintf(stderr, "event_loop_init: unknown CEXPRESS_EVENT_LOOP=\"%s\" (expected epoll or io_uring)\n", forced);
        return -1;
    }

    if (forced != NULL && strcmp(forced, "io_uring") == 0) {
#if defined(__linux__) && !defined(CEXPRESS_USE_EPOLL)
        const int rc = io_uring_loop_ops.init(app);
        if (rc == 0) {
            app->loop_ops = &io_uring_loop_ops;
            return 0;
        }
        if (rc == EVENT_LOOP_UNAVAILABLE) {
            fprintf(stderr, "event_loop_init: CEXPRESS_EVENT_LOOP=io_uring, but io_uring is unavailable (%s)\n",
                    strerror(errno));
        }
        return -1;
#else
        fprintf(stderr, "event_loop_init: CEXPRESS_EVENT_LOOP=io_uring, but this build has no io_uring backend\n");
        return -1;
#endif
    }

    if (epoll_loop_ops.init(app) != 0) {
        return -1;
    }
    app->loop_ops = &epoll_loop_ops;
    return 0;
}

const char *event_loop_backend_name(const App *app) {
    return (app != NULL && app->loop_ops != NULL) ? app->loop_ops->name : "none";
}

int event_loop_is_open(const App *app) {
    return app != NULL && app->loop_ops != NULL && app->loop_ops->is_open(app);
}

void event_loop_close(App *app) {
    if (app == NULL || app->loop_ops == NULL) {
        return;
    }
    app->loop_ops->close_loop(app);
    app->loop_ops = NULL;
}

int event_loop_watch_read(App *app, int fd, void *udata) {
    return (app != NULL && app->loop_ops != NULL) ? app->loop_ops->watch_read(app, fd, udata) : -1;
}

int event_loop_unwatch_read(App *app, int fd) {
    return (app != NULL && app->loop_ops != NULL) ? app->loop_ops->unwatch_read(app, fd) : -1;
}

int event_loop_watch_write(App *app, int fd, void *udata) {
    return (app != NULL && app->loop_ops != NULL) ? app->loop_ops->watch_write(app, fd, udata) : -1;
}

int event_loop_unwatch_write(App *app, int fd, void *udata) {
    return (app != NULL && app->loop_ops != NULL) ? app->loop_ops->unwatch_write(app, fd, udata) : -1;
}

int event_loop_release_fd(App *app, int fd) {
    return (app != NULL && app->loop_ops != NULL) ? app->loop_ops->release_fd(app, fd) : -1;
}

int event_loop_arm_shutdown_timer(App *app) {
    return (app != NULL && app->loop_ops != NULL) ? app->loop_ops->arm_shutdown_timer(app) : -1;
}

int event_loop_poll(App *app, LoopEvent *out_events, int max_events, int timeout_ms) {
    if (app == NULL || app->loop_ops == NULL) {
        errno = EBADF; /* app_listen_worker treats EBADF during shutdown as "loop closed" */
        return -1;
    }
    return app->loop_ops->poll_events(app, out_events, max_events, timeout_ms);
}

#endif /* defined(__linux__) || defined(CEXPRESS_USE_EPOLL) */
