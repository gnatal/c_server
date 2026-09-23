#if defined(__linux__) || defined(CEXPRESS_USE_EPOLL)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "event_loop.h"
#include "event_loop_backend.h"

/*
 * C5: runtime backend selection. io_uring is preferred; when the kernel or the container runtime refuses
 * to create a ring (EVENT_LOOP_UNAVAILABLE), the worker logs why and runs on epoll instead of exiting.
 * CEXPRESS_EVENT_LOOP=epoll|io_uring forces one (no fallback when forced); any other non-empty value is
 * refused. Built with CEXPRESS_USE_EPOLL (Linux NO_URING=1, or macOS epoll-shim), epoll is the only one.
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

#if defined(__linux__) && !defined(CEXPRESS_USE_EPOLL)
    /* Whether io_uring was refused is a property of the kernel and sandbox, fixed for the process's life:
     * after the first refusal, automatic selection goes straight to epoll (no failing syscall and no
     * repeated log line per init; a cluster worker inherits nothing, the master never opens a loop). */
    static int uring_refused;
    if (forced != NULL ? strcmp(forced, "io_uring") == 0 : !uring_refused) {
        const int rc = io_uring_loop_ops.init(app);
        if (rc == 0) {
            app->loop_ops = &io_uring_loop_ops;
            return 0;
        }
        if (rc != EVENT_LOOP_UNAVAILABLE) {
            return -1;
        }
        if (forced != NULL) {
            fprintf(stderr, "event_loop_init: io_uring unavailable (%s) and CEXPRESS_EVENT_LOOP=io_uring forbids "
                            "falling back\n", strerror(errno));
            return -1;
        }
        fprintf(stderr, "event_loop_init: io_uring unavailable (%s), falling back to epoll\n", strerror(errno));
        uring_refused = 1;
    }
#else
    if (forced != NULL && strcmp(forced, "io_uring") == 0) {
        fprintf(stderr, "event_loop_init: CEXPRESS_EVENT_LOOP=io_uring, but this build has no io_uring backend\n");
        return -1;
    }
#endif

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

int event_loop_unwatch_all(App *app, int fd) {
    return (app != NULL && app->loop_ops != NULL) ? app->loop_ops->unwatch_all(app, fd) : -1;
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
