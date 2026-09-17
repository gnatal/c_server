#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <errno.h>
#include "app_types.h"
#include "event_loop.h"
#include "connection.h"
#include "router.h"

static void test_event_loop_lifecycle(void) {
    App app;
    app_init(&app);

    assert(event_loop_init(&app) == 0);
    assert(app.loop_fd >= 0);

    event_loop_close(&app);
    assert(app.loop_fd == -1);

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

int main(void) {
    test_event_loop_lifecycle();
    test_event_loop_watch_read_write();
    test_event_loop_shutdown_timer_arm();

    printf("all event loop tests passed\n");
    return 0;
}
