#ifndef EVENT_LOOP_H
#define EVENT_LOOP_H

#include "app_types.h"

/*
 * Initializes the event loop subsystem. macOS/BSD: kqueue. Linux: io_uring, falling back at runtime
 * to epoll (logged on stderr) when the kernel or container refuses to create a ring - seccomp, the
 * kernel.io_uring_disabled sysctl, RLIMIT_MEMLOCK, a pre-5.1 kernel. CEXPRESS_EVENT_LOOP=epoll|io_uring
 * forces one on Linux (no fallback; any other non-empty value fails); it is ignored by kqueue builds.
 * NO_URING=1 (Makefile, -DCEXPRESS_USE_EPOLL) builds epoll only, without liburing.
 * Sets up signal interception for SIGINT and SIGTERM, and registers the
 * periodic idle-connection sweep timer (IDLE_SWEEP_INTERVAL_MS).
 * Returns 0 on success, -1 on failure (every backend failed, or the forced one did).
 */
int event_loop_init(App *app);

/* The open backend: "kqueue", "io_uring" or "epoll"; "none" when no loop is open. Static string. */
const char *event_loop_backend_name(const App *app);

/*
 * 1 between a successful event_loop_init and event_loop_close, else 0. The backend handle shares a
 * union in App (kq / epoll_fd / ring), so callers must ask this instead of testing App.loop_fd: on
 * io_uring that field is half of a pointer and can read as negative while the loop is open.
 */
int event_loop_is_open(const App *app);

/*
 * Closes the event loop descriptor and any auxiliary descriptors (timerfds,
 * signalfds on Linux), and restores default dispositions and masks for
 * SIGINT and SIGTERM. Safe and idempotent.
 */
void event_loop_close(App *app);

/*
 * Registers interest in read readiness on the given descriptor (EVFILT_READ on
 * kqueue, EPOLLIN on epoll, a multishot POLLIN poll on io_uring). udata is passed through to event notifications.
 * Returns 0 on success, -1 on failure.
 */
int event_loop_watch_read(App *app, int fd, void *udata);

/*
 * Drops interest in read readiness on the given descriptor.
 * Returns 0 on success, -1 on failure.
 */
int event_loop_unwatch_read(App *app, int fd);

/*
 * Registers interest in write readiness on the given descriptor (EVFILT_WRITE on
 * kqueue, EPOLLOUT on epoll). Preserves any existing read interest.
 * Returns 0 on success, -1 on failure.
 */
int event_loop_watch_write(App *app, int fd, void *udata);

/*
 * Drops interest in write readiness on the given descriptor, preserving read
 * interest.
 * Returns 0 on success, -1 on failure.
 */
int event_loop_unwatch_write(App *app, int fd, void *udata);

/*
 * Deregisters all event interest on the given descriptor (both read and write).
 * Returns 0 on success, -1 on failure.
 */
int event_loop_unwatch_all(App *app, int fd);

/*
 * Arms a oneshot shutdown deadline timer (SHUTDOWN_TIMEOUT_SECONDS) on the event
 * loop to bound maximum connection drain time during graceful shutdown.
 * Returns 0 on success, -1 on failure.
 */
int event_loop_arm_shutdown_timer(App *app);

/*
 * Blocks waiting for events up to timeout_ms (-1 for indefinite wait).
 * Populates out_events with normalized LoopEvent structures, up to max_events.
 * Returns the number of events ready, 0 on timeout, or -1 on error. -1 with errno EINTR is not an error:
 * poll again (on Linux, epoll_wait returns it spuriously after the process has torn down an io_uring
 * ring; SIGINT/SIGTERM never cause it - they arrive as LOOP_EVENT_SIGNAL).
 */
int event_loop_poll(App *app, LoopEvent *out_events, int max_events, int timeout_ms);

#endif /* EVENT_LOOP_H */
