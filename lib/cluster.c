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

typedef struct {
    pid_t pid;
    int worker_id;
    int active;
} ClusterWorkerSlot;

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

    ClusterWorkerSlot workers[MAX_CLUSTER_WORKERS];
    memset(workers, 0, sizeof(workers));

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
        pid_t pid = spawn_worker(app, port, i);
        if (pid > 0) {
            workers[i].pid = pid;
            workers[i].worker_id = i;
            workers[i].active = 1;
        } else {
            fprintf(stderr, "Failed to spawn cluster worker %d\n", i);
        }
    }

    /* Master supervision loop */
    while (!g_shutdown_requested) {
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
                        fprintf(stderr, "Master: Worker %d (PID %d) terminated by signal %d, respawning...\n",
                                slot, (int)exited_pid, termsig);
                    } else {
                        fprintf(stderr, "Master: Worker %d (PID %d) exited with code %d, respawning...\n",
                                slot, (int)exited_pid, exit_code);
                    }
                    pid_t new_pid = spawn_worker(app, port, slot);
                    if (new_pid > 0) {
                        workers[slot].pid = new_pid;
                        workers[slot].active = 1;
                    }
                } else {
                    workers[slot].active = 0;
                }
            }
        } else {
            struct timespec req = {0, 50000000L}; /* 50ms sleep */
            nanosleep(&req, NULL);
        }
    }

    /* Graceful cluster shutdown sequence */
    printf("\nMaster received signal %d, draining %d workers...\n",
           g_shutdown_signo, workers_count);

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
}
