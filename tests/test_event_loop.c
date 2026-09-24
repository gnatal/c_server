#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <errno.h>
#include "app_types.h"
#include "event_loop.h"
#include "connection.h"
#include "router.h"

static void test_event_loop_lifecycle(void) {
    App app;
    app_init(&app);

    assert(strcmp(event_loop_backend_name(&app), "none") == 0);
    assert(event_loop_init(&app) == 0);
    /* which backend is live decides which union member is meaningful. */
    const char *backend = event_loop_backend_name(&app);
#if defined(__linux__) && !defined(CEXPRESS_USE_EPOLL)
    if (strcmp(backend, "io_uring") == 0) {
        assert(app.ring != NULL);
    } else {
        assert(strcmp(backend, "epoll") == 0 && app.epoll_fd >= 0);
    }
#elif defined(__linux__) || defined(CEXPRESS_USE_EPOLL)
    assert(strcmp(backend, "epoll") == 0 && app.epoll_fd >= 0);
#else
    assert(strcmp(backend, "kqueue") == 0 && app.loop_fd >= 0);
#endif

    event_loop_close(&app);
    assert(strcmp(event_loop_backend_name(&app), "none") == 0);

    /* Idempotent close */
    event_loop_close(&app);
    app_destroy(&app);
}

static void test_event_loop_watch_read_write(void) {
    App app;
    app_init(&app);
    assert(event_loop_init(&app) == 0);

    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    /* Watch read on fds[0] */
    assert(event_loop_watch_read(&app, fds[0], NULL) == 0);

    /* Poll with nothing written: should time out (0 events) */
    LoopEvent events[8];
    int n = event_loop_poll(&app, events, 8, 50);
    assert(n == 0);

    /* Write 5 bytes into fds[1] */
    const char *msg = "hello";
    assert(write(fds[1], msg, 5) == 5);

    /* Poll: fds[0] should be readable */
    n = event_loop_poll(&app, events, 8, 200);
    assert(n > 0);
    int read_found = 0;
    for (int i = 0; i < n; i++) {
        if (events[i].fd == fds[0] && events[i].type == LOOP_EVENT_READ) {
            read_found = 1;
        }
    }
    assert(read_found == 1);

    /* Read the bytes to clear readability */
    char buf[16];
    assert(read(fds[0], buf, sizeof(buf)) == 5);

    /* Watch write on fds[0] */
    assert(event_loop_watch_write(&app, fds[0], NULL) == 0);

    /* Poll: fds[0] should now be writable */
    n = event_loop_poll(&app, events, 8, 200);
    assert(n > 0);
    int write_found = 0;
    for (int i = 0; i < n; i++) {
        if (events[i].fd == fds[0] && events[i].type == LOOP_EVENT_WRITE) {
            write_found = 1;
        }
    }
    assert(write_found == 1);

    /* Unwatch write */
    assert(event_loop_unwatch_write(&app, fds[0], NULL) == 0);

    /* Unwatch all */
    assert(event_loop_unwatch_all(&app, fds[0]) == 0);

    close(fds[0]);
    close(fds[1]);
    app_destroy(&app);
}

static void test_event_loop_shutdown_timer_arm(void) {
    App app;
    app_init(&app);
    assert(event_loop_init(&app) == 0);

    assert(event_loop_arm_shutdown_timer(&app) == 0);

    app_destroy(&app);
}

/* ---- backend contract the engine relies on (found broken on io_uring only) ---- */

/* A Connection registered in app->connections, as accept_connections would, so the backends track
 * events_watched for it. Freed with plain free (no connection_close: nothing else is attached). */
static Connection *register_fake_connection(App *app, const int fd) {
    Connection *conn = calloc(1, sizeof(Connection));
    assert(conn != NULL);
    conn->fd = fd;
    conn->file_fd = -1;
    assert(fd < app->connections_cap);
    app->connections[fd] = conn;
    return conn;
}

static int count_events(const LoopEvent *events, const int n, const int fd, const LoopEventType type) {
    int count = 0;
    for (int i = 0; i < n; i++) {
        if (events[i].fd == fd && events[i].type == type) {
            count++;
        }
    }
    return count;
}

/* Regression: changing interest (what flush_connection / wait_for_writable do around every
 * response) must never surface as LOOP_EVENT_ERROR. On io_uring the removed poll's -ECANCELED
 * completion was reported as an error and connection.c closed the connection. */
static void test_interest_changes_are_never_errors(void) {
    App app;
    app_init(&app);
    assert(event_loop_init(&app) == 0);
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    assert(set_nonblocking(fds[0]) == 0);
    Connection *conn = register_fake_connection(&app, fds[0]);
    assert(event_loop_watch_read(&app, fds[0], conn) == 0);

    LoopEvent events[16];
    for (int round = 0; round < 50; round++) {
        /* the keep-alive cycle: a partial write arms write interest and drops read, the drained
         * response drops write and re-arms read, then the redundant unwatch_write every response does */
        assert(event_loop_watch_write(&app, fds[0], conn) == 0);
        assert(event_loop_unwatch_read(&app, fds[0]) == 0);
        const int n1 = event_loop_poll(&app, events, 16, 20);
        assert(n1 >= 0 && count_events(events, n1, fds[0], LOOP_EVENT_ERROR) == 0);
        assert(event_loop_unwatch_write(&app, fds[0], conn) == 0);
        assert(event_loop_watch_read(&app, fds[0], conn) == 0);
        assert(event_loop_unwatch_write(&app, fds[0], conn) == 0);

        assert(write(fds[1], "x", 1) == 1);
        int reads = 0;
        for (int tries = 0; tries < 10 && reads == 0; tries++) {
            const int n = event_loop_poll(&app, events, 16, 100);
            assert(n >= 0);
            assert(count_events(events, n, fds[0], LOOP_EVENT_ERROR) == 0);
            reads += count_events(events, n, fds[0], LOOP_EVENT_READ);
        }
        assert(reads > 0);
        char c;
        assert(read(fds[0], &c, 1) == 1);
    }

    assert(event_loop_unwatch_all(&app, fds[0]) == 0);
    app.connections[fds[0]] = NULL;
    free(conn);
    close(fds[0]);
    close(fds[1]);
    app_destroy(&app);
}

/* A watch/unwatch that would not change the tracked interest set must not reach the kernel: every
 * keep-alive response calls unwatch_write with write never armed. The fd is tracked but was never
 * registered, so any syscall fails with ENOENT (-1 returned) where the skipped no-op returns 0. */
static void test_noop_interest_changes_skip_the_kernel(void) {
    App app;
    app_init(&app);
    assert(event_loop_init(&app) == 0);
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    Connection *conn = register_fake_connection(&app, fds[0]);

    assert(event_loop_unwatch_write(&app, fds[0], conn) == 0); /* nothing watched */
    assert(event_loop_unwatch_read(&app, fds[0]) == 0);
    assert(conn->events_watched == 0);

    conn->events_watched = EVENT_READ; /* tracked as read-only, unknown to the kernel */
    assert(event_loop_unwatch_write(&app, fds[0], conn) == 0);
    assert(event_loop_watch_read(&app, fds[0], conn) == 0);
    assert(conn->events_watched == EVENT_READ);

    conn->events_watched = EVENT_WRITE; /* tracked as write-only, unknown to the kernel */
    assert(event_loop_unwatch_read(&app, fds[0]) == 0);
    assert(event_loop_watch_write(&app, fds[0], conn) == 0);
    assert(conn->events_watched == EVENT_WRITE);

    assert(event_loop_unwatch_all(&app, fds[0]) == 0);
    app.connections[fds[0]] = NULL;
    free(conn);
    close(fds[0]);
    close(fds[1]);
    app_destroy(&app);
}

/* connection.c assumes level-triggered readiness: it serves one request and returns, expecting to be
 * called again while unread bytes remain, and yields a still-writable socket expecting a write event
 * next turn. Readiness left in place must be reported again on every poll. */
static void test_readiness_is_level_triggered(void) {
    App app;
    app_init(&app);
    assert(event_loop_init(&app) == 0);
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    Connection *conn = register_fake_connection(&app, fds[0]);
    assert(event_loop_watch_read(&app, fds[0], conn) == 0);
    assert(write(fds[1], "unread", 6) == 6);

    LoopEvent events[16];
    for (int poll_round = 0; poll_round < 3; poll_round++) {
        const int n = event_loop_poll(&app, events, 16, 200);
        assert(count_events(events, n, fds[0], LOOP_EVENT_READ) == 1); /* bytes never consumed */
    }

    assert(event_loop_unwatch_read(&app, fds[0]) == 0);
    assert(event_loop_watch_write(&app, fds[0], conn) == 0);
    for (int poll_round = 0; poll_round < 3; poll_round++) {
        const int n = event_loop_poll(&app, events, 16, 200);
        assert(count_events(events, n, fds[0], LOOP_EVENT_WRITE) == 1); /* socket stays writable */
        assert(count_events(events, n, fds[0], LOOP_EVENT_READ) == 0);  /* read interest was dropped */
    }

    assert(event_loop_unwatch_all(&app, fds[0]) == 0);
    app.connections[fds[0]] = NULL;
    free(conn);
    close(fds[0]);
    close(fds[1]);
    app_destroy(&app);
}

/* A connection closed with readiness pending must not leak that readiness to the next connection that
 * gets the same fd number (io_uring: a stale completion carries the old generation and is dropped). */
static void test_reused_fd_gets_no_stale_events(void) {
    App app;
    app_init(&app);
    assert(event_loop_init(&app) == 0);
    int old_pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, old_pair) == 0);
    Connection *old_conn = register_fake_connection(&app, old_pair[0]);
    assert(write(old_pair[1], "pending", 7) == 7);
    assert(event_loop_watch_read(&app, old_pair[0], old_conn) == 0);
    LoopEvent events[16];
    const int n0 = event_loop_poll(&app, events, 16, 200);
    assert(count_events(events, n0, old_pair[0], LOOP_EVENT_READ) == 1);

    const int reused_fd = old_pair[0];
    assert(event_loop_unwatch_all(&app, reused_fd) == 0); /* what connection_close does before close */
    app.connections[reused_fd] = NULL;
    free(old_conn);
    close(old_pair[0]);
    close(old_pair[1]);

    int new_pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, new_pair) == 0);
    int new_fd = new_pair[0];
    if (new_fd != reused_fd) {
        /* fds are handed out lowest-first, so this is the reused number unless something else took it */
        new_fd = new_pair[1];
    }
    Connection *new_conn = register_fake_connection(&app, new_fd);
    assert(event_loop_watch_read(&app, new_fd, new_conn) == 0);
    const int n = event_loop_poll(&app, events, 16, 100);
    assert(count_events(events, n, new_fd, LOOP_EVENT_READ) == 0);  /* nothing was sent to it */
    assert(count_events(events, n, new_fd, LOOP_EVENT_ERROR) == 0);

    assert(event_loop_unwatch_all(&app, new_fd) == 0);
    app.connections[new_fd] = NULL;
    free(new_conn);
    close(new_pair[0]);
    close(new_pair[1]);
    app_destroy(&app);
}

static void test_event_loop_is_open(void) {
    App app;
    app_init(&app);
    assert(!event_loop_is_open(&app));
    assert(event_loop_init(&app) == 0);
    assert(event_loop_is_open(&app));
    event_loop_close(&app);
    assert(!event_loop_is_open(&app));
    app_destroy(&app);
}

#if defined(__linux__) || defined(CEXPRESS_USE_EPOLL)
/* CEXPRESS_EVENT_LOOP selection in event_loop_linux.c. A forced backend never silently becomes
 * another one, and an unknown value fails instead of guessing. */
static void test_backend_selection_env(void) {
    App app;
    app_init(&app);

    assert(setenv("CEXPRESS_EVENT_LOOP", "kqueue-please", 1) == 0);
    assert(event_loop_init(&app) == -1);
    assert(!event_loop_is_open(&app) && strcmp(event_loop_backend_name(&app), "none") == 0);

    assert(setenv("CEXPRESS_EVENT_LOOP", "epoll", 1) == 0);
    assert(event_loop_init(&app) == 0);
    assert(event_loop_is_open(&app) && strcmp(event_loop_backend_name(&app), "epoll") == 0);
    event_loop_close(&app);
    assert(!event_loop_is_open(&app));

    /* io_uring forced: it is io_uring or nothing (unavailable here, or not built in). */
    assert(setenv("CEXPRESS_EVENT_LOOP", "io_uring", 1) == 0);
    if (event_loop_init(&app) == 0) {
        assert(strcmp(event_loop_backend_name(&app), "io_uring") == 0);
        event_loop_close(&app);
    } else {
        assert(!event_loop_is_open(&app) && strcmp(event_loop_backend_name(&app), "none") == 0);
    }

    /* Empty means unset: automatic selection. */
    assert(setenv("CEXPRESS_EVENT_LOOP", "", 1) == 0);
    assert(event_loop_init(&app) == 0);
    event_loop_close(&app);

    assert(unsetenv("CEXPRESS_EVENT_LOOP") == 0);
    app_destroy(&app);
}
#endif

/* Every backend-contract test, against whatever event_loop_init selects under the current environment. */
static void run_contract_tests(void) {
    test_event_loop_lifecycle();
    test_event_loop_watch_read_write();
    test_event_loop_shutdown_timer_arm();
    test_interest_changes_are_never_errors();
    test_noop_interest_changes_skip_the_kernel();
    test_readiness_is_level_triggered();
    test_reused_fd_gets_no_stale_events();
    test_event_loop_is_open();
}

static const char *selected_backend(void) {
    static App app;
    app_init(&app);
    assert(event_loop_init(&app) == 0);
    static char name[16];
    snprintf(name, sizeof(name), "%s", event_loop_backend_name(&app));
    event_loop_close(&app);
    app_destroy(&app);
    return name;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0); /* keep progress lines visible if an assert aborts */
    /* The caller's CEXPRESS_EVENT_LOOP (if any) picks the backend for the first pass. */
    printf("event loop tests: backend %s\n", selected_backend());
    run_contract_tests();

#if defined(__linux__) || defined(CEXPRESS_USE_EPOLL)
    test_backend_selection_env();
#if defined(__linux__) && !defined(CEXPRESS_USE_EPOLL)
    /* epoll is now a runtime fallback on every Linux build, so it gets the whole contract too, even
     * when io_uring was available for the first pass. */
    /* In a fresh child: tearing down an io_uring ring queues task_work on the process, which can make its
     * next blocking syscall - here epoll_wait - return a spurious EINTR (seen on 6.8). The engine retries
     * EINTR (app_listen_worker), and a real process never runs both backends (the fallback happens only
     * when no ring could be created), so the epoll pass gets a process that never had a ring. */
    if (strcmp(selected_backend(), "epoll") != 0) {
        const pid_t child = fork();
        assert(child >= 0);
        if (child == 0) {
            assert(setenv("CEXPRESS_EVENT_LOOP", "epoll", 1) == 0);
            printf("event loop tests: backend %s\n", selected_backend());
            run_contract_tests();
            _exit(0);
        }
        int status = 0;
        assert(waitpid(child, &status, 0) == child);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
#endif
#endif

    printf("all event loop tests passed\n");
    return 0;
}
