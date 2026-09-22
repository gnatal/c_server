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

static int g_is_worker = 0;
static int g_worker_id = -1;

static volatile sig_atomic_t g_shutdown_requested = 0;
static volatile sig_atomic_t g_shutdown_signo = 0;

/*
 * S7 restart backoff/budget: a slot that keeps exiting abnormally is respawned with an increasing
 * delay instead of instantly, and after CLUSTER_RESTART_BUDGET failures within
 * CLUSTER_RESTART_WINDOW_MS the master gives up on the whole cluster rather than forking forever -
 * MEASURED (improvements.md, S7) 10,594 respawns and 31,788 log lines in about 4 seconds with
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

static pid_t spawn_worker(App *app, int port, int worker_id) {
    /* fork() duplicates the master's stdio buffers as-is: an unflushed line (stdout to a pipe/file is
     * fully buffered, not line-buffered) would otherwise be flushed a second time by the child's own
     * exit(), printing it twice. Immaterial for one worker at startup, but S7's respawn loop can fork
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

        /* Run isolated worker event loop */
        app_listen_worker(app, port);

        /* Teardown worker resources and exit */
        app_destroy(app);
        exit(EXIT_SUCCESS);
    }

    return pid;
}

void cluster_listen(App *app, int port, int num_workers) {
    int workers_count = cluster_resolve_worker_count(num_workers);
    if (workers_count <= 1) {
        app_listen_worker(app, port);
        return;
    }

    /*
     * S7: verify the port is actually usable once, in the master, before forking anyone. Without this,
     * a fatal misconfiguration (port already in use, permission denied on a privileged port, ...) made
     * every one of workers_count children fail create_server_socket's own bind()/listen() identically
     * and immediately, and the old respawn-on-exit loop re-forked each one right away - MEASURED
     * 10,594 respawns in about 4 seconds with WORKERS=2 and the port already taken.
     * create_server_socket already prints one clear perror() and calls exit() (S12: still unfixed -
     * it exits rather than returning an error code, so this call doubles as the "better" fix
     * improvements.md suggests: verify the listen socket before forking, so a fatal configuration
     * error stops the whole server once) on any such failure, so calling it here for the sole purpose
     * of validating and then discarding the fd turns what used to be a fork storm into that single
     * message. */
    close(create_server_socket(port));

    ClusterWorkerSlot workers[MAX_CLUSTER_WORKERS];
    memset(workers, 0, sizeof(workers));
    int fatal = 0;

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

    printf("CExpress cluster master (PID %d) starting %d workers on port %d\n",
           (int)getpid(), workers_count, port);

    for (int i = 0; i < workers_count; i++) {
        workers[i].worker_id = i;
        pid_t pid = spawn_worker(app, port, i);
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
                    pid_t new_pid = spawn_worker(app, port, i);
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
            struct timespec req = {0, 50000000L}; /* 50ms sleep */
            nanosleep(&req, NULL);
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

    printf("All workers terminated cleanly. Master exiting.\n");

    if (fatal) {
        /* S7/S12: cluster_listen is void, with no way to hand a failure back to app_listen/main() -
         * same "library calls exit() for a fatal condition" convention create_server_socket and
         * event_loop_init already use (S12: still unfixed there either - a real error-code path is a
         * larger, separate change). A non-zero status here is what lets an orchestrator (systemd,
         * Docker, Kubernetes) see the server as failed and act on it, instead of the process quietly
         * running with fewer workers than requested or exiting 0 as if nothing happened. */
        exit(EXIT_FAILURE);
    }
}
