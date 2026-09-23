#ifndef EVENT_LOOP_BACKEND_H
#define EVENT_LOOP_BACKEND_H

#include "app_types.h"

/*
 * Private to the Linux event loop (C5): event_loop_linux.c implements the public event_loop.h API by
 * forwarding to one of these tables, chosen at runtime by event_loop_init and stored in App.loop_ops.
 * Each backend's functions are static; its table is the only symbol it exports. kqueue (macOS/BSD) is
 * the only backend there and implements event_loop.h directly, without a table.
 * Every entry has the same contract as the event_loop.h function of the same name, except `init`:
 * it returns EVENT_LOOP_UNAVAILABLE (errno set) when the kernel mechanism itself cannot be created
 * (io_uring_setup refused: ENOSYS, EPERM under seccomp or kernel.io_uring_disabled, ENOMEM under a small
 * RLIMIT_MEMLOCK) and nothing was touched - the dispatcher then tries the next backend. Any other failure
 * is -1 (already cleaned up) and is final.
 */
#define EVENT_LOOP_UNAVAILABLE (-2)

typedef struct EventLoopOps {
    const char *name; /* "io_uring" / "epoll": what event_loop_backend_name reports */
    int (*init)(App *app);
    int (*is_open)(const App *app);
    void (*close_loop)(App *app);   /* not "close"/"poll": epoll-shim #defines those names */
    int (*watch_read)(App *app, int fd, void *udata);
    int (*unwatch_read)(App *app, int fd);
    int (*watch_write)(App *app, int fd, void *udata);
    int (*unwatch_write)(App *app, int fd, void *udata);
    int (*unwatch_all)(App *app, int fd);
    int (*arm_shutdown_timer)(App *app);
    int (*poll_events)(App *app, LoopEvent *out_events, int max_events, int timeout_ms);
} EventLoopOps;

extern const EventLoopOps epoll_loop_ops;    /* event_loop_epoll.c: Linux, or macOS via epoll-shim */
#if defined(__linux__) && !defined(CEXPRESS_USE_EPOLL)
extern const EventLoopOps io_uring_loop_ops; /* event_loop_io_uring.c: Linux unless built with NO_URING=1 */
#endif

#endif /* EVENT_LOOP_BACKEND_H */
