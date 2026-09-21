# Concurrency & Multi-Worker Architecture

CExpress employs a **multi-process worker model** driven by kernel-level `SO_REUSEPORT` socket load balancing, master process supervision, automatic worker crash recovery, and synchronized graceful shutdown.

---

## 1. Architectural Overview

Rather than relying on thread pools with shared mutable state, mutexes, and race conditions, CExpress follows the shared-nothing, multi-process architecture utilized by industry-standard servers like **Nginx**, **HAProxy**, and Node.js (`cluster` module).

```
┌─────────────────────────────────────────────────────────────┐
│                    Master Process (PID M)                   │
│   - Supervises worker processes                             │
│   - Acts as PID 1 init supervisor in Docker                 │
│   - Tracks worker table: pid_t workers[MAX_CLUSTER_WORKERS] │
│   - Intercepts SIGINT/SIGTERM, forwards to workers          │
│   - Reaps dead children (waitpid) preventing zombie leaks   │
│   - Automatically respawns crashed workers                  │
└──────────────┬───────────────────────────────┬──────────────┘
               │ forks (SO_REUSEPORT)          │ forks (SO_REUSEPORT)
┌──────────────▼──────────────┐ ┌──────────────▼──────────────┐
│       Worker 0 (PID W0)     │ │       Worker 1 (PID W1)     │
│ - Dedicated SO_REUSEPORT fd │ │ - Dedicated SO_REUSEPORT fd │
│ - Private kqueue/io_uring   │ │ - Private kqueue/io_uring   │
│   event loop                │ │   event loop                │
│ - Private connection table  │ │ - Private connection table  │
│ - Zero-lock request routing │ │ - Zero-lock request routing │
│ - Drains & exits on SIGTERM │ │ - Drains & exits on SIGTERM │
└─────────────────────────────┘ └─────────────────────────────┘
```

### Key Architectural Invariants
1. **Zero Lock Contention**: Each worker process executes an isolated single-threaded event loop (`kqueue` on macOS/BSD, `io_uring` readiness polling on Linux) with its own private connection table and memory space. Request routing, HTTP parsing, and response serialization remain completely lock-free.
2. **Total Fault Isolation**: A crash, assertion failure, or segmentation fault in one worker process cannot corrupt the memory space or bring down other workers or the master supervisor.
3. **Hardware Scaling**: The design lets throughput scale with CPU cores, but linear scaling has not been demonstrated in this repository. On an Apple M3 Pro with `wrk` on the same machine, `GET /ping` at 100 connections reached about 222k req/s with 1 worker and about 250k with 4 (single runs, 21 Sep 2026): the load generator competes for the same cores, so those runs are client-bound. Measure on separate machines before quoting a scaling factor.

---

## 2. Kernel `SO_REUSEPORT` Mechanics

Traditional UNIX networking allows only one process to bind to a given IP/port. With `SO_REUSEPORT`:
* Multiple independent sockets on the same machine can bind to the exact same IP and TCP port.
* Each worker process opens, configures, and binds its own dedicated listening socket:
  ```c
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
  bind(server_fd, ...);
  listen(server_fd, BACKLOG);
  ```

### Platform-Specific Kernel Behavior

| Feature | macOS / BSD | Linux |
|---|---|---|
| **Distribution** | Intended: incoming connections are accepted by available workers. Observed: uneven, see below. | Kernel performs 4-tuple hashing (`src_ip`, `src_port`, `dst_ip`, `dst_port`) across worker sockets (documented kernel behavior, not measured here). |
| **Accept Queues** | Independent per-socket accept queue. | Lockless per-socket accept queue in kernel network stack. |
| **Thundering Herd** | Prevented: each worker only wakes up when its own socket is ready. | Prevented: kernel wakes only the specific socket selected by the hash. |
| **Overhead** | Zero inter-process communication (IPC) on the network path. | Zero IPC overhead; zero cross-worker locks. |

`test_cluster` checks that several workers bind the same port and serve requests concurrently.

**Measured on macOS (21 Sep 2026): connections were not balanced.** With `WORKERS=4` and 5,000 keep-alive connections opened from one client, one worker process held nearly all of them: its resident memory was 124 MB of the 134 MB total across the master and four workers (at 100 connections, 7.7 of 17.1 MB). The macOS column above therefore describes the intended `SO_REUSEPORT` behavior, not what the kernel did in this test; treat multi-worker mode on macOS as a development convenience, not a scaling mechanism, until that is understood. The Linux distribution (4-tuple hashing) was not measured here.

---

## 3. How It Works in Docker & Container Environments

The multi-process architecture is specifically engineered to operate cleanly and reliably in containerized environments (Docker, Podman, Kubernetes):

### A. PID 1 & Zombie Reaping in Docker
* In a Docker container, the binary specified in `CMD ["/app/cexpress_demo"]` is assigned **PID 1** inside the container's PID namespace.
* Standard POSIX processes become zombies if their parent process does not explicitly call `waitpid()`. In typical applications, dead child processes can leak kernel resources.
* **Master as Init Supervisor**:
  * The CExpress master process acts as a robust init system for the container.
  * It installs a `SIGCHLD` handler and its supervision loop (waking every 50 ms) reaps dead child processes using `waitpid(-1, &status, WNOHANG)`.
  * No external init system (such as `tini` or `dumb-init`) is required.

### B. Container Network Namespaces
* Inside the Docker container, all processes share the container's private network namespace (`eth0` and `lo`).
* Linux kernel `SO_REUSEPORT` works natively within network namespaces:
  1. Docker forwards host traffic (`-p 8080:8080`) into `eth0:8080` inside the container.
  2. The Linux kernel distributes incoming TCP SYN packets across the container workers' sockets.
  3. Workers accept connections directly from their individual event loops (`io_uring` on Linux) with zero virtualization overhead inside the container.

### C. Dynamic CPU Detection
* In container orchestration (Docker / Kubernetes), CPU limits are configured via `--cpus` or CPU quotas.
* When `WORKERS=auto` or `WORKERS=0` is set:
  * CExpress calls `sysconf(_SC_NPROCESSORS_ONLN)`.
  * It detects the actual number of online CPU cores allocated to the container environment and spawns the exact number of workers needed to saturate hardware.

### D. io_uring in Containers
* The Linux build polls for readiness with `io_uring` (liburing, multishot poll: kernel 5.13 or newer). The `Dockerfile` installs `liburing-dev` in the builder and `liburing` in the runtime image.
* If `io_uring_queue_init` fails (for example because the container runtime's seccomp profile blocks the `io_uring_*` syscalls), `event_loop_init` returns `-1` and `app_listen_worker` prints the error and exits with a failure status. There is no runtime fallback to `epoll`. In cluster mode the master treats that as an abnormal worker exit and respawns the worker (from reading `lib/cluster.c`; a blocked-io_uring crash loop was not reproduced).
* `scripts/docker_stress_test.sh` runs the server with `--security-opt seccomp=unconfined --ulimit memlock=-1:-1`. Whether Docker's default profile actually blocks io_uring on a given host was not tested for this document; if the server exits immediately in a container, try those flags first.
* An `epoll` backend (`lib/event_loop_epoll.c`, selected by `-DCEXPRESS_USE_EPOLL`) is kept in the tree but is not wired into the Linux `Makefile`.

### E. Orchestration Signals & Rolling Updates
* When `docker stop` or Kubernetes pod termination occurs, the container runtime sends `SIGTERM` to PID 1 (the master process).
* The master process catches `SIGTERM` and coordinates graceful connection draining across all workers within the stop timeout (default 10s in Docker, 5s deadline in CExpress).

---

## 4. Worker Lifecycle & Fault Tolerance

```
           [Master Starts]
                  │
                  ▼
         [Spawn N Workers]
                  │
        ┌─────────┴─────────┐
        ▼                   ▼
  [Normal Running]    [Worker Crashes]
        │                   │
        │             Master catches SIGCHLD
        │             via waitpid()
        │                   │
        │             Respawn Replacement Worker
        │             (Zero Downtime)
        │                   │
        └─────────┬─────────┘
                  │
         SIGINT / SIGTERM
                  │
                  ▼
     [Master Signals Workers]
                  │
                  ▼
     [Workers Close Server FDs]
     [Drains In-Flight Requests]
     [5s Graceful Deadline]
                  │
                  ▼
     [All Workers Terminate Cleanly]
                  │
                  ▼
           [Master Exits 0]
```

### Crash Detection & Automatic Respawning
If a worker terminates abnormally (e.g. unhandled segmentation fault or non-zero exit code) while the server is running:
1. The master process receives `SIGCHLD`.
2. `waitpid(-1, &status, WNOHANG)` identifies the exited worker PID and slot.
3. Master logs the incident:
   ```text
   Master: Worker 2 (PID 4821) terminated by signal 11, respawning...
   ```
4. Master immediately forks a new replacement worker in that slot. The replacement worker creates its own `SO_REUSEPORT` socket, initializes its event loop, and resumes accepting traffic.

### Synchronized Graceful Shutdown
1. When master receives `SIGTERM` (e.g. from `kill <master_pid>` or `docker stop`):
   * Sets `g_shutdown_requested = 1`.
   * Broadcasts `SIGTERM` to all active worker PIDs.
2. When master receives `SIGINT` (e.g. `Ctrl+C` in a terminal):
   * The terminal kernel driver broadcasts `SIGINT` to the entire process group.
   * Master recognizes `SIGINT` and coordinates draining without sending redundant signals.
3. Each worker enters `app_stop()`:
   * Closes its listening socket immediately (stops accepting new connections).
   * Drains in-flight HTTP requests and streams.
   * Enforces a 5-second deadline timer.
   * Calls `app_destroy()` and exits with code 0.
4. Master waits for all worker PIDs to exit with `waitpid()`.
5. Master safety watchdog: if any worker does not terminate within 6 seconds, master sends `SIGKILL` as a safeguard before exiting cleanly with code 0.

---

## 5. Usage & Configuration

### Environment Variables
| Variable | Value | Description |
|---|---|---|
| `WORKERS` | `auto` or `0` | Automatically spawns workers equal to the number of available CPU cores. |
| `WORKERS` | `N` (e.g. `4`) | Spawns exactly $N$ worker processes (up to `MAX_CLUSTER_WORKERS = 128`). |
| `WORKERS` | `1` (or unset)| Runs in single-process mode (default). |

`WORKERS` is read by the demo app (`examples/todo_sqlite/main.c`); the engine itself uses `app.config.workers`, where `0` means one per CPU core and values above `MAX_CLUSTER_WORKERS` are clamped.

### Example CLI Usage
```bash
# From examples/todo_sqlite/ after `make demo` at the repository root.
# Auto-detect CPU cores (e.g. 8 cores -> 8 workers):
WORKERS=auto ./cexpress_demo

# Explicitly launch 4 workers:
WORKERS=4 ./cexpress_demo

# In Docker:
docker run -e WORKERS=auto -p 8080:8080 cexpress
```

### C API Usage
```c
#include "cexpress.h"

int main(void) {
    App app;
    app_init(&app);

    /* Configure routes and middlewares */
    app_get(&app, "/", handler_home);

    /* Programmatic cluster configuration */
    app.config.workers = 4;

    /* Automatically launches 4 workers */
    app_listen(&app, 8080);

    /* Or explicitly launch cluster: */
    /* cluster_listen(&app, 8080, 4); */

    app_destroy(&app);
    return 0;
}
```
