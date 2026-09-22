#ifndef CLUSTER_H
#define CLUSTER_H

#include "app_types.h"

#define MAX_CLUSTER_WORKERS 128

/*
 * Resolves the requested worker count:
 * - If requested <= 0: queries system CPU core count via sysconf(_SC_NPROCESSORS_ONLN).
 * - If requested > MAX_CLUSTER_WORKERS: clamps to MAX_CLUSTER_WORKERS.
 * - If sysconf returns <= 0 or fails: falls back to 1.
 * - Otherwise returns requested.
 */
int cluster_resolve_worker_count(int requested);

/*
 * Returns 1 if the current process is executing as a cluster worker, 0 if master
 * or running in standalone single-process mode.
 */
int cluster_is_worker(void);

/*
 * Returns the 0-indexed worker ID (0 to num_workers - 1) if running as a worker,
 * or -1 if master / standalone.
 */
int cluster_worker_id(void);

/*
 * Starts the multi-process cluster supervisor:
 * - If num_workers <= 1: runs app_listen_worker directly in the current process.
 * - If num_workers > 1: verifies the port is bindable once (create_server_socket, which exits with a
 *   clear error on failure - S7/S12) before forking anyone, then forks num_workers child processes,
 *   each opening its own independent SO_REUSEPORT listening socket and event loop.
 * - The master supervisor intercepts SIGINT/SIGTERM, forwards termination signals
 *   to workers, reaps child processes (waitpid) preventing zombies, and respawns any worker that
 *   terminates unexpectedly during normal operation - with exponential backoff per slot, and a
 *   restart budget: a slot that keeps failing beyond the budget makes the whole master drain and
 *   exit(EXIT_FAILURE) instead of forking forever (S7; see lib/CLAUDE.md, "Behavior reference,
 *   Workers and fork" for the exact numbers).
 */
void cluster_listen(App *app, int port, int num_workers);

#endif /* CLUSTER_H */
