#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "cluster.h"
#include "connection.h"
#ifdef CEXPRESS_SINGLE_ACCEPTOR
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#endif

static int g_is_worker = 0;
static int g_worker_id = -1;

static volatile sig_atomic_t g_shutdown_requested = 0;
static volatile sig_atomic_t g_shutdown_signo = 0;

/*
 * Restart backoff/budget: a slot that keeps exiting abnormally is respawned with an increasing
 * delay instead of instantly, and after CLUSTER_RESTART_BUDGET failures within
 * CLUSTER_RESTART_WINDOW_MS the master gives up on the whole cluster rather than forking forever -
 * MEASURED (improvements.md) 10,594 respawns and 31,788 log lines in about 4 seconds with
 * WORKERS=2 and the port already taken. Implementation-only constants, never exposed through
 * cluster.h.
 */
#define CLUSTER_RESTART_BACKOFF_INITIAL_MS 100  /* delay before the 1st respawn attempt after a failure */
#define CLUSTER_RESTART_BACKOFF_MAX_MS 30000    /* backoff doubles per consecutive failure, capped here */
#define CLUSTER_RESTART_BUDGET 5                /* failures allowed per slot within the window below */
#define CLUSTER_RESTART_WINDOW_MS 60000         /* sliding window the budget above is measured over */

typedef struct {
    pid_t pid;
    int worker_id;
    int active;             /* has a live pid this master is tracking */
    int pending_respawn;    /* abnormal exit (or a failed respawn attempt): waiting out backoff_until_ms */
    long long backoff_until_ms;   /* monotonic time the next respawn attempt for this slot is allowed */
    int consecutive_failures;     /* failures counted within the current restart-budget window */
    long long window_start_ms;    /* when the current window started; 0 = no window yet */
    /* CEXPRESS_SINGLE_ACCEPTOR only: the master-side end (sv[0]) of this slot's socketpair with
     * its worker, used to hand it accepted client fds via SCM_RIGHTS (dispatch_client_fd). -1 when no
     * worker is currently running in this slot. Unused (always -1) on the non-single-acceptor path. */
    int control_fd;
} ClusterWorkerSlot;

static long long monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * Accounts one failure for `slot` against its restart budget: resets the sliding window if the last
 * failure fell outside it, otherwise extends it. Schedules an exponential-backoff respawn
 * (pending_respawn / backoff_until_ms) unless the budget within the window is exhausted, in which case
 * it returns 1 (fatal: the caller must stop the whole cluster) and leaves the slot inactive for good.
 * Shared by an abnormal worker exit and a fork() failure while trying to (re)spawn one, so both count
 * against the same budget instead of one of them looping unbounded.
 */
static int record_worker_failure(ClusterWorkerSlot *w, int slot) {
    const long long now = monotonic_ms();

    if (w->window_start_ms == 0 || now - w->window_start_ms > CLUSTER_RESTART_WINDOW_MS) {
        w->window_start_ms = now;
        w->consecutive_failures = 1;
    } else {
        w->consecutive_failures++;
    }
    w->active = 0;

    if (w->consecutive_failures > CLUSTER_RESTART_BUDGET) {
        fprintf(stderr, "Master: worker %d failed %d times within %ds, giving up - shutting down\n",
                slot, w->consecutive_failures, CLUSTER_RESTART_WINDOW_MS / 1000);
        w->pending_respawn = 0;
        return 1;
    }

    long long backoff_ms = CLUSTER_RESTART_BACKOFF_INITIAL_MS;
    for (int i = 1; i < w->consecutive_failures; i++) {
        if (backoff_ms >= CLUSTER_RESTART_BACKOFF_MAX_MS) {
            backoff_ms = CLUSTER_RESTART_BACKOFF_MAX_MS;
            break;
        }
        backoff_ms *= 2;
    }
    fprintf(stderr, "Master: will respawn worker %d in %lldms (failure %d/%d in this window)\n",
            slot, backoff_ms, w->consecutive_failures, CLUSTER_RESTART_BUDGET);
    w->pending_respawn = 1;
    w->backoff_until_ms = now + backoff_ms;
    return 0;
}

int cluster_resolve_worker_count(int requested) {
    if (requested <= 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        if (n <= 0) {
            return 1;
        }
        if (n > MAX_CLUSTER_WORKERS) {
            return MAX_CLUSTER_WORKERS;
        }
        return (int)n;
    }
    if (requested > MAX_CLUSTER_WORKERS) {
        return MAX_CLUSTER_WORKERS;
    }
    return requested;
}

int cluster_is_worker(void) {
    return g_is_worker;
}

int cluster_worker_id(void) {
    return g_worker_id;
}

static void master_signal_handler(int signo) {
    if (signo == SIGINT || signo == SIGTERM) {
        g_shutdown_requested = 1;
        g_shutdown_signo = signo;
    }
}

/* Shared by both spawn_worker variants below: the fork()+child-teardown skeleton every worker slot
 * uses. `run_child` does whatever is left to set server_fd up (bind its own SO_REUSEPORT socket, or - single
 * acceptor - close everything but its own control socket and use that) before app_listen_worker's event
 * loop starts. */
static pid_t spawn_worker_common(App *app, int worker_id, void (*run_child)(App *app, int port, void *ctx),
                                  int port, void *ctx) {
    /* fork() duplicates the master's stdio buffers as-is: an unflushed line (stdout to a pipe/file is
     * fully buffered, not line-buffered) would otherwise be flushed a second time by the child's own
     * exit(), printing it twice. Immaterial for one worker at startup, but the respawn loop can fork
     * the same slot many times in quick succession, and duplicated log lines are exactly the kind of
     * noise a crash loop should not add to. */
    fflush(stdout);
    fflush(stderr);

    pid_t pid = fork();
    if (pid < 0) {
        perror("cluster: fork");
        return -1;
    }

    if (pid == 0) {
        /* In child worker process */
        g_is_worker = 1;
        g_worker_id = worker_id;

        /* Reset signal handlers to default dispositions so that the native
         * event loop (kqueue EVFILT_SIGNAL or Linux signalfd) manages
         * signals synchronously without interference from master. */
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_DFL;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGCHLD, &sa, NULL);

        run_child(app, port, ctx);

        /* Teardown worker resources and exit */
        app_destroy(app);
        exit(EXIT_SUCCESS);
    }

    return pid;
}

#ifdef CEXPRESS_SINGLE_ACCEPTOR
/* fd-passing context handed through spawn_worker_common to the child branch - everything the
 * child needs to close (fds it inherited via fork() but does not need) and its own control fd. */
typedef struct {
    int listen_fd;                 /* the master's real listen socket - never needed by a worker */
    ClusterWorkerSlot *workers;    /* every slot's *master-side* control_fd, to close all but this one */
    int workers_count;
    int slot_idx;                  /* this worker's own slot, and the fd (worker_control_fd) to keep */
    int worker_control_fd;         /* sv[1]: this worker's own control socket */
} FdPassingChildCtx;

static void run_child_fd_passing(App *app, int port, void *ctx_ptr) {
    (void)port;
    FdPassingChildCtx *ctx = (FdPassingChildCtx *)ctx_ptr;

    /* Close every fd this child inherited via fork() but does not need: the master's real listen
     * socket (this worker never accept()s directly), and every *other* slot's master-side control
     * fd (this worker must not be able to read or write another worker's inbound fd-handoff channel
     * - leaving those open would let a buggy or malicious worker inject fabricated SCM_RIGHTS
     * messages into a sibling's control socket). */
    close(ctx->listen_fd);
    for (int j = 0; j < ctx->workers_count; j++) {
        if (j != ctx->slot_idx && ctx->workers[j].control_fd >= 0) {
            close(ctx->workers[j].control_fd);
        }
    }

    app_listen_worker_via_control_socket(app, ctx->worker_control_fd);
}

/* single-acceptor variant. Creates a fresh AF_UNIX socketpair for this slot before forking - the
 * parent keeps sv[0] (control_fd, used to hand this worker client fds) and the child keeps sv[1]
 * (passed to app_listen_worker_via_control_socket). Called both for the initial spawn and for every
 * respawn: a dead worker's old sv[1] died with its process, so each attempt needs its own pair. */
static pid_t spawn_worker(App *app, int listen_fd, ClusterWorkerSlot *workers, int workers_count, int slot_idx) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        perror("cluster: socketpair");
        return -1;
    }
    /* Both ends must be non-blocking, matching the event loop's readiness model: the worker's
     * accept_passed_connections drains recvmsg() in a loop until EAGAIN, the same "poll says
     * readable, read until it isn't" contract every other watched fd follows (event_loop_*.c). On a
     * blocking socket, the loop's final recvmsg call after draining every pending message would
     * block forever instead of returning EAGAIN, freezing this worker's entire single-threaded event
     * loop. Setting it here, before fork(), applies to both processes' views of the same underlying
     * open file descriptions (O_NONBLOCK is a file-status flag, shared across fork() same as it is
     * across SCM_RIGHTS/dup()). The master's own dispatch_client_fd sendmsg calls don't strictly need
     * this (the control socket's send buffer is normally far from full), but non-blocking keeps a
     * slow/stuck worker from ever stalling the master's own accept loop either. */
    set_nonblocking(sv[0]);
    set_nonblocking(sv[1]);

    FdPassingChildCtx ctx = {
        .listen_fd = listen_fd,
        .workers = workers,
        .workers_count = workers_count,
        .slot_idx = slot_idx,
        .worker_control_fd = sv[1],
    };
    pid_t pid = spawn_worker_common(app, slot_idx, run_child_fd_passing, 0, &ctx);
    if (pid < 0) {
        close(sv[0]);
        close(sv[1]);
        return -1;
    }

    /* Parent: only the master-side end is ours to keep. */
    close(sv[1]);
    workers[slot_idx].control_fd = sv[0];
    return pid;
}

/* hands one accepted client fd to worker `*next` (or the next active one after it) via
 * SCM_RIGHTS over its control socket, round-robin. A one-byte data payload rides along with the
 * ancillary data - a zero-length SCM_RIGHTS-only message is ill-defined on some AF_UNIX
 * implementations; the worker's recvmsg reads and discards this byte, only the cmsg fd matters.
 * Returns 1 and advances *next past the worker it used on success, 0 if no active worker could take
 * it (every slot down, or every sendmsg failed - e.g. mid crash-loop with no live worker at all). */
static int dispatch_client_fd(ClusterWorkerSlot *workers, int workers_count, int *next, int client_fd) {
    for (int tries = 0; tries < workers_count; tries++) {
        int i = (*next + tries) % workers_count;
        if (!workers[i].active || workers[i].control_fd < 0) {
            continue;
        }

        char data_byte = 'x';
        struct iovec iov = { .iov_base = &data_byte, .iov_len = 1 };
        char cmsg_buf[CMSG_SPACE(sizeof(int))];
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cmsg_buf;
        msg.msg_controllen = sizeof(cmsg_buf);

        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &client_fd, sizeof(int));

        if (sendmsg(workers[i].control_fd, &msg, 0) >= 0) {
            *next = (i + 1) % workers_count;
            return 1;
        }
    }
    return 0;
}
#else
static void run_child_direct_listen(App *app, int port, void *ctx) {
    (void)ctx;
    /* Run isolated worker event loop: binds its own SO_REUSEPORT socket (create_server_socket, via
     * app_listen_worker) and accept()s directly. */
    app_listen_worker(app, port);
}

static pid_t spawn_worker(App *app, int port, int worker_id) {
    return spawn_worker_common(app, worker_id, run_child_direct_listen, port, NULL);
}
#endif

void cluster_listen(App *app, int port, int num_workers) {
    int workers_count = cluster_resolve_worker_count(num_workers);
    if (workers_count <= 1) {
        app_listen_worker(app, port);
        return;
    }

    /*
     * verify the port is actually usable once, in the master, before forking anyone. Without this,
     * a fatal misconfiguration (port already in use, permission denied on a privileged port, ...) made
     * every one of workers_count children fail create_server_socket's own bind()/listen() identically
     * and immediately, and the old respawn-on-exit loop re-forked each one right away - MEASURED
     * 10,594 respawns in about 4 seconds with WORKERS=2 and the port already taken.
     * create_server_socket itself just returns -1 on failure now (it no longer exit()s on its
     * own); this call site is the one that decides a preflight failure is fatal and turns it into a
     * single clear message + exit(), which still gives the same "one message, no fork storm" behavior
     * this comment originally described. */
#ifdef CEXPRESS_SINGLE_ACCEPTOR
    /* kept open (not closed like the preflight-only check below) - this is the one real listen
     * socket for the whole cluster's lifetime; only the master accepts on it. */
    int listen_fd = create_server_socket(port);
    if (listen_fd < 0) {
        fprintf(stderr, "cluster_listen: could not create listening socket on port %d\n", port);
        exit(EXIT_FAILURE);
    }
#else
    int preflight_fd = create_server_socket(port);
    if (preflight_fd < 0) {
        fprintf(stderr, "cluster_listen: could not create listening socket on port %d\n", port);
        exit(EXIT_FAILURE);
    }
    close(preflight_fd);
#endif

    ClusterWorkerSlot workers[MAX_CLUSTER_WORKERS];
    memset(workers, 0, sizeof(workers));
    for (int i = 0; i < MAX_CLUSTER_WORKERS; i++) {
        workers[i].control_fd = -1; /* the plain memset above would otherwise leave fd 0 (stdin) here */
    }
    int fatal = 0;
#ifdef CEXPRESS_SINGLE_ACCEPTOR
    int next_worker = 0; /* round-robin cursor for dispatch_client_fd, persists across loop iterations */
#endif

    g_shutdown_requested = 0;
    g_shutdown_signo = 0;

    /* Install master process signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = master_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGCHLD, &sa, NULL);

#ifdef CEXPRESS_SINGLE_ACCEPTOR
    /* the master now itself writes to worker control sockets (dispatch_client_fd's sendmsg); a
     * worker that has already closed its end (draining, or dead but not yet reaped) must fail that
     * call with EPIPE, not take down the master with the default SIGPIPE disposition - workers
     * already ignore it themselves (app_listen_worker), the master never had to before this. */
    signal(SIGPIPE, SIG_IGN);
#endif

    printf("CExpress cluster master (PID %d) starting %d workers on port %d\n",
           (int)getpid(), workers_count, port);
#ifdef CEXPRESS_SINGLE_ACCEPTOR
    /* the master itself owns the one real listen socket now (each worker used to print this
     * line for itself, from inside its own bind - see app_listen_worker's now-suppressed print when
     * accept_via_fd_passing is set). */
    printf("Listening on port %d\n", port);
#endif

    for (int i = 0; i < workers_count; i++) {
        workers[i].worker_id = i;
#ifdef CEXPRESS_SINGLE_ACCEPTOR
        pid_t pid = spawn_worker(app, listen_fd, workers, workers_count, i);
#else
        pid_t pid = spawn_worker(app, port, i);
#endif
        if (pid > 0) {
            workers[i].pid = pid;
            workers[i].active = 1;
        } else {
            fprintf(stderr, "Failed to spawn cluster worker %d, will retry with backoff\n", i);
            if (record_worker_failure(&workers[i], i)) {
                fatal = 1;
            }
        }
    }

    /* Master supervision loop */
    while (!g_shutdown_requested && !fatal) {
        int status = 0;
        pid_t exited_pid = waitpid(-1, &status, WNOHANG);

        if (exited_pid > 0) {
            int slot = -1;
            for (int i = 0; i < workers_count; i++) {
                if (workers[i].active && workers[i].pid == exited_pid) {
                    slot = i;
                    break;
                }
            }

            if (slot >= 0) {
                int abnormal = 0;
                int exit_code = 0;
                int termsig = 0;

                if (WIFEXITED(status)) {
                    exit_code = WEXITSTATUS(status);
                    if (exit_code != 0) {
                        abnormal = 1;
                    }
                } else if (WIFSIGNALED(status)) {
                    termsig = WTERMSIG(status);
                    abnormal = 1;
                }

                if (abnormal && !g_shutdown_requested) {
                    if (termsig != 0) {
                        fprintf(stderr, "Master: Worker %d (PID %d) terminated by signal %d\n",
                                slot, (int)exited_pid, termsig);
                    } else {
                        fprintf(stderr, "Master: Worker %d (PID %d) exited with code %d\n",
                                slot, (int)exited_pid, exit_code);
                    }
                    if (record_worker_failure(&workers[slot], slot)) {
                        fatal = 1;
                    }
                } else {
                    workers[slot].active = 0;
                }
            }
        }

        /* Respawn any slot whose backoff has elapsed, independent of whether a pid exited this tick -
         * backoff can run to CLUSTER_RESTART_BACKOFF_MAX_MS (30s) while this loop polls every ~50ms. */
        if (!g_shutdown_requested && !fatal) {
            const long long now = monotonic_ms();
            for (int i = 0; i < workers_count; i++) {
                if (workers[i].pending_respawn && now >= workers[i].backoff_until_ms) {
                    workers[i].pending_respawn = 0;
#ifdef CEXPRESS_SINGLE_ACCEPTOR
                    /* The dead worker's own sv[1] died with its process; the master's sv[0] copy for
                     * this slot is still open and must be closed before spawn_worker overwrites it
                     * with a fresh pair below, or it leaks one fd per respawn. */
                    if (workers[i].control_fd >= 0) {
                        close(workers[i].control_fd);
                        workers[i].control_fd = -1;
                    }
                    pid_t new_pid = spawn_worker(app, listen_fd, workers, workers_count, i);
#else
                    pid_t new_pid = spawn_worker(app, port, i);
#endif
                    if (new_pid > 0) {
                        workers[i].pid = new_pid;
                        workers[i].active = 1;
                    } else {
                        fprintf(stderr, "Failed to respawn cluster worker %d, will retry with backoff\n", i);
                        if (record_worker_failure(&workers[i], i)) {
                            fatal = 1;
                        }
                    }
                }
            }
        }

        if (exited_pid <= 0) {
#ifdef CEXPRESS_SINGLE_ACCEPTOR
            /* wait on listen_fd instead of an unconditional sleep - poll() returns immediately
             * once a connection is pending, so this adds no latency over the old per-worker accept()
             * path, while still guaranteeing the loop wakes at least every 50ms for the waitpid/
             * backoff scan above, same cadence the plain nanosleep gave it before. */
            struct pollfd pfd = { .fd = listen_fd, .events = POLLIN, .revents = 0 };
            int pr = poll(&pfd, 1, 50);
            if (pr > 0 && (pfd.revents & POLLIN)) {
                while (1) {
                    const int client_fd = accept_client(listen_fd); /* non-blocking + TCP_NODELAY */
                    if (client_fd < 0) {
                        break; /* EAGAIN (drained) or a transient error: stop draining this wake */
                    }
                    /* Closed whether or not dispatch succeeded. On success SCM_RIGHTS gave the worker
                     * its own reference to the socket, and the master's copy must go: while it stays
                     * open the worker's close() never sends a FIN (Connection: close and idle
                     * timeouts don't end the TCP stream), and the master leaks one fd per connection
                     * until accept() hits EMFILE and the whole cluster stops taking connections.
                     * On failure no active worker could take it (e.g. mid crash-loop, every slot
                     * down): best-effort shed, same philosophy as accept_connections' own overload
                     * handling - deliberately not duplicating reject_overloaded_connection's
                     * hand-built 503 here in the master to keep the single acceptor simple. */
                    (void)dispatch_client_fd(workers, workers_count, &next_worker, client_fd);
                    close(client_fd);
                }
            }
#else
            struct timespec req = {0, 50000000L}; /* 50ms sleep */
            nanosleep(&req, NULL);
#endif
        }
    }

    if (fatal && g_shutdown_signo == 0) {
        /* Reuse the signal-driven drain sequence below as-is: a restart-budget failure is the master
         * asking everyone to stop, the same as an external SIGTERM would, just triggered internally. */
        g_shutdown_signo = SIGTERM;
    }

    /* Graceful cluster shutdown sequence */
    if (fatal) {
        printf("\nMaster: draining %d workers before exiting (restart budget exceeded)...\n", workers_count);
    } else {
        printf("\nMaster received signal %d, draining %d workers...\n", g_shutdown_signo, workers_count);
    }

    /* If shutdown was triggered via SIGTERM (e.g. kill or docker stop targeting master PID),
     * forward SIGTERM to all workers. If triggered via SIGINT from terminal, the process group
     * already received SIGINT from the kernel. */
    if (g_shutdown_signo == SIGTERM) {
        for (int i = 0; i < workers_count; i++) {
            if (workers[i].active && workers[i].pid > 0) {
                kill(workers[i].pid, SIGTERM);
            }
        }
    }

    /* Wait for workers to drain within 6 seconds */
    time_t start_time = time(NULL);
    while (1) {
        int any_active = 0;
        for (int i = 0; i < workers_count; i++) {
            if (workers[i].active && workers[i].pid > 0) {
                int status = 0;
                pid_t res = waitpid(workers[i].pid, &status, WNOHANG);
                if (res == workers[i].pid || (res < 0 && errno == ECHILD)) {
                    workers[i].active = 0;
                } else {
                    any_active = 1;
                }
            }
        }

        if (!any_active) {
            break;
        }

        /* Safety deadline: if workers haven't exited after 6 seconds, send SIGKILL */
        if (time(NULL) - start_time >= 6) {
            fprintf(stderr, "Master: Worker drain timeout exceeded (6s), sending SIGKILL\n");
            for (int i = 0; i < workers_count; i++) {
                if (workers[i].active && workers[i].pid > 0) {
                    kill(workers[i].pid, SIGKILL);
                    waitpid(workers[i].pid, NULL, 0);
                    workers[i].active = 0;
                }
            }
            break;
        }

        struct timespec req = {0, 50000000L}; /* 50ms sleep */
        nanosleep(&req, NULL);
    }

#ifdef CEXPRESS_SINGLE_ACCEPTOR
    /* Every worker has exited by this point (the drain-wait loop above only exits once none are
     * active), so any still-open control_fd is just the master's own unclosed copy - close it here
     * rather than leaking it past cluster_listen's return. */
    close(listen_fd);
    for (int i = 0; i < workers_count; i++) {
        if (workers[i].control_fd >= 0) {
            close(workers[i].control_fd);
            workers[i].control_fd = -1;
        }
    }
#endif

    printf("All workers terminated cleanly. Master exiting.\n");

    if (fatal) {
        /* cluster_listen is void, with no way to hand a failure back to app_listen/main() - the
         * restart budget being exhausted is decided and acted on right here rather than propagated as a
         * return code (a real error-code path all the way back to main() is a larger, separate change;
         * create_server_socket itself no longer forces this - it now just returns -1 and this file's
         * callers are the ones that choose to exit()). A non-zero status here is what lets an orchestrator
         * (systemd, Docker, Kubernetes) see the server as failed and act on it, instead of the process
         * quietly running with fewer workers than requested or exiting 0 as if nothing happened. */
        exit(EXIT_FAILURE);
    }
}
