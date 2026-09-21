#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <time.h>
#include "cexpress.h"
#include "cluster.h"

static void test_cluster_resolve_worker_count(void) {
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu <= 0) {
        ncpu = 1;
    }
    if (ncpu > MAX_CLUSTER_WORKERS) {
        ncpu = MAX_CLUSTER_WORKERS;
    }

    assert(cluster_resolve_worker_count(0) == (int)ncpu);
    assert(cluster_resolve_worker_count(-1) == (int)ncpu);
    assert(cluster_resolve_worker_count(1) == 1);
    assert(cluster_resolve_worker_count(4) == 4);
    assert(cluster_resolve_worker_count(MAX_CLUSTER_WORKERS) == MAX_CLUSTER_WORKERS);
    assert(cluster_resolve_worker_count(MAX_CLUSTER_WORKERS + 50) == MAX_CLUSTER_WORKERS);
}

static void test_cluster_worker_identification(void) {
    /* Standalone / test process is not a cluster worker */
    assert(cluster_is_worker() == 0);
    assert(cluster_worker_id() == -1);
}

static void test_so_reuseport_multi_bind(void) {
    int s1 = socket(AF_INET, SOCK_STREAM, 0);
    assert(s1 >= 0);

    const int opt = 1;
    assert(setsockopt(s1, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == 0);
#ifdef SO_REUSEPORT
    assert(setsockopt(s1, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) == 0);
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0); /* ephemeral port */

    assert(bind(s1, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(listen(s1, 16) == 0);

    socklen_t len = sizeof(addr);
    assert(getsockname(s1, (struct sockaddr *)&addr, &len) == 0);
    int assigned_port = ntohs(addr.sin_port);
    assert(assigned_port > 0);

    /* Second socket binding to the exact same IP and port */
    int s2 = socket(AF_INET, SOCK_STREAM, 0);
    assert(s2 >= 0);
    assert(setsockopt(s2, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == 0);
#ifdef SO_REUSEPORT
    assert(setsockopt(s2, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) == 0);
#endif
    assert(bind(s2, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(listen(s2, 16) == 0);

    close(s1);
    close(s2);
}

static void ping_handler(const Request *req, Response *res) {
    (void)req;
    res_send(res, "pong");
}

static int get_ephemeral_port(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    assert(s >= 0);

    const int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);

    assert(bind(s, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    assert(getsockname(s, (struct sockaddr *)&addr, &len) == 0);
    int port = ntohs(addr.sin_port);
    close(s);
    return port;
}

static void test_cluster_http_serving_and_shutdown(void) {
    int test_port = get_ephemeral_port();

    pid_t master_pid = fork();
    assert(master_pid >= 0);

    if (master_pid == 0) {
        /* In test master process */
        App app;
        app_init(&app);
        app.config.workers = 2;
        app_get(&app, "/ping", ping_handler);

        /* Redirection removed for debugging */

        app_listen(&app, test_port);
        app_destroy(&app);
        exit(0);
    }

    /* In parent test runner process: wait for cluster to initialize */
    struct timespec delay = {0, 200000000L}; /* 200ms */
    nanosleep(&delay, NULL);

    /* Issue HTTP requests to the cluster */
    int client_fd = -1;
    for (int retry = 0; retry < 10; retry++) {
        client_fd = socket(AF_INET, SOCK_STREAM, 0);
        assert(client_fd >= 0);

        struct sockaddr_in srv_addr;
        memset(&srv_addr, 0, sizeof(srv_addr));
        srv_addr.sin_family = AF_INET;
        srv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        srv_addr.sin_port = htons(test_port);

        if (connect(client_fd, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) == 0) {
            break;
        }
        close(client_fd);
        client_fd = -1;
        nanosleep(&delay, NULL);
    }
    assert(client_fd >= 0);

    const char *req_str = "GET /ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ssize_t sent = write(client_fd, req_str, strlen(req_str));
    assert(sent == (ssize_t)strlen(req_str));

    char buf[1024];
    memset(buf, 0, sizeof(buf));
    ssize_t nread = read(client_fd, buf, sizeof(buf) - 1);
    assert(nread > 0);
    assert(strstr(buf, "200 OK") != NULL);
    assert(strstr(buf, "pong") != NULL);
    close(client_fd);

    /* Send SIGTERM to the cluster master */
    assert(kill(master_pid, SIGTERM) == 0);

    /* Wait for cluster master to cleanly terminate */
    int status = 0;
    pid_t waited = waitpid(master_pid, &status, 0);
    assert(waited == master_pid);
    if (!WIFEXITED(status)) {
        printf("Master process did not exit normally. WIFSIGNALED: %d, WTERMSIG: %d\n", WIFSIGNALED(status), WTERMSIG(status));
        fflush(stdout);
    }
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
}

int main(void) {
    test_cluster_resolve_worker_count();
    test_cluster_worker_identification();
    test_so_reuseport_multi_bind();
    test_cluster_http_serving_and_shutdown();

    printf("all cluster tests passed\n");
    return 0;
}
