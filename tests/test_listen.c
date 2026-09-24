/*
 * The listening socket's address (ServerConfig.bind_address, create_server_socket). Behind a TLS proxy the
 * plaintext port must not be reachable from other hosts: a listener bound to 127.0.0.1 has to refuse a
 * connection to this host's own non-loopback address, which the old always-INADDR_ANY listener accepted.
 * Real sockets on loopback plus, when the host has one, its first non-loopback IPv4 address (the test that
 * needs it is skipped with a note otherwise); IPv6 cases are skipped when the host has no IPv6 loopback.
 */
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include "cexpress.h"
#include "connection.h"

static void ping_handler(const Request *req, Response *res) {
    (void)req;
    res_send(res, "pong");
}

/* the port a listener got (it binds port 0), whatever its family. */
static int bound_port(const int fd) {
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    assert(getsockname(fd, (struct sockaddr *)&ss, &len) == 0);
    return ss.ss_family == AF_INET6 ? ntohs(((struct sockaddr_in6 *)&ss)->sin6_port)
                                    : ntohs(((struct sockaddr_in *)&ss)->sin_port);
}

/* 1 when a blocking TCP connect to ip:port succeeds, 0 when it is refused (or otherwise fails). */
static int can_connect(const char *ip, const int port) {
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof(ss));
    socklen_t len;
    if (strchr(ip, ':') != NULL) {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)&ss;
        a->sin6_family = AF_INET6;
        a->sin6_port = htons((unsigned short)port);
        assert(inet_pton(AF_INET6, ip, &a->sin6_addr) == 1);
        len = sizeof(*a);
    } else {
        struct sockaddr_in *a = (struct sockaddr_in *)&ss;
        a->sin_family = AF_INET;
        a->sin_port = htons((unsigned short)port);
        assert(inet_pton(AF_INET, ip, &a->sin_addr) == 1);
        len = sizeof(*a);
    }
    const int fd = socket(ss.ss_family, SOCK_STREAM, 0);
    assert(fd >= 0);
    const int ok = connect(fd, (struct sockaddr *)&ss, len) == 0;
    close(fd);
    return ok;
}

/* this host's first up, non-loopback IPv4 address into out (INET_ADDRSTRLEN), 0 when it has none. */
static int non_loopback_ipv4(char *out) {
    struct ifaddrs *ifs = NULL;
    if (getifaddrs(&ifs) != 0) return 0;
    int found = 0;
    for (const struct ifaddrs *i = ifs; i != NULL && !found; i = i->ifa_next) {
        if (i->ifa_addr == NULL || i->ifa_addr->sa_family != AF_INET) continue;
        const struct in_addr a = ((const struct sockaddr_in *)i->ifa_addr)->sin_addr;
        if ((ntohl(a.s_addr) >> 24) == 127) continue;
        found = inet_ntop(AF_INET, &a, out, INET_ADDRSTRLEN) != NULL;
    }
    freeifaddrs(ifs);
    return found;
}

static int host_has_ipv6_loopback(void) {
    const int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_in6 a;
    memset(&a, 0, sizeof(a));
    a.sin6_family = AF_INET6;
    a.sin6_addr = in6addr_loopback;
    const int ok = bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0;
    close(fd);
    return ok;
}

static void test_app_init_defaults_to_every_ipv4_interface(void) {
    App app;
    memset(&app, 0xAB, sizeof(app)); /* app_init must not rely on a zeroed App */
    app_init(&app);
    assert(app.config.bind_address == NULL);
    app_destroy(&app);
}

/* NULL keeps the historical listener: 0.0.0.0, IPv4. */
static void test_null_binds_every_ipv4_interface(void) {
    const int fd = create_server_socket(NULL, 0);
    assert(fd >= 0);
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    assert(getsockname(fd, (struct sockaddr *)&a, &len) == 0);
    assert(a.sin_family == AF_INET);
    assert(a.sin_addr.s_addr == htonl(INADDR_ANY));
    assert(can_connect("127.0.0.1", bound_port(fd)));
    close(fd);
}

/* the fix: 127.0.0.1 is reachable on loopback only. The same connection to the host's own non-loopback
 * address that a NULL (0.0.0.0) listener accepts is refused. */
static void test_loopback_bind_refuses_other_interfaces(void) {
    const int fd = create_server_socket("127.0.0.1", 0);
    assert(fd >= 0);
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    assert(getsockname(fd, (struct sockaddr *)&a, &len) == 0);
    assert(a.sin_family == AF_INET && a.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
    const int port = bound_port(fd);
    assert(can_connect("127.0.0.1", port));

    char other[INET_ADDRSTRLEN];
    if (!non_loopback_ipv4(other)) {
        printf("test_listen: no non-loopback IPv4 address, skipping the other-interface check\n");
        close(fd);
        return;
    }
    assert(!can_connect(other, port));
    close(fd);

    const int any_fd = create_server_socket(NULL, 0); /* control: that address does reach a 0.0.0.0 listener */
    assert(any_fd >= 0);
    assert(can_connect(other, bound_port(any_fd)));
    close(any_fd);
}

/* "::1" is IPv6 loopback only; "::" is every interface, IPv4 included (IPV6_V6ONLY off on every platform). */
static void test_ipv6_addresses(void) {
    if (!host_has_ipv6_loopback()) {
        printf("test_listen: no IPv6 loopback, skipping IPv6 cases\n");
        return;
    }
    const int v6_loop = create_server_socket("::1", 0);
    assert(v6_loop >= 0);
    assert(can_connect("::1", bound_port(v6_loop)));
    assert(!can_connect("127.0.0.1", bound_port(v6_loop)));
    close(v6_loop);

    const int dual = create_server_socket("::", 0);
    assert(dual >= 0);
    assert(can_connect("::1", bound_port(dual)));
    assert(can_connect("127.0.0.1", bound_port(dual)));
    close(dual);
}

/* numeric literals only: a hostname would mean a DNS lookup at startup and, for a name with several
 * addresses, binding just one of them. */
static void test_non_numeric_addresses_rejected(void) {
    const char *bad[] = {"localhost", "", "256.0.0.1", "127.0.0.1 ", " 127.0.0.1", "1.2.3", "127.1", "0x7f.0.0.1",
                         "::g", "::1%", "example.com", "127.0.0.1:80", "[::1]"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        assert(create_server_socket(bad[i], 0) == -1);
    }
}

/* a free loopback port for a forked server (bound once, then released). */
static int free_port(void) {
    const int fd = create_server_socket("127.0.0.1", 0);
    assert(fd >= 0);
    const int port = bound_port(fd);
    close(fd);
    return port;
}

static int get_ping(const char *ip, const int port) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    assert(inet_pton(AF_INET, ip, &a.sin_addr) == 1);
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(fd);
        return 0;
    }
    const char *req = "GET /ping HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    assert(write(fd, req, strlen(req)) == (ssize_t)strlen(req));
    char buf[512] = {0};
    const ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    return n > 0 && strstr(buf, "pong") != NULL;
}

/* app_listen honors config.bind_address on every path that binds: app_listen_worker (workers = 1) and
 * cluster_listen (workers = 2: the macOS single acceptor's master socket, or each Linux worker's own). */
static void test_app_listen_uses_bind_address(const int workers) {
    const int port = free_port();
    const pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        App app;
        app_init(&app);
        app.config.workers = workers;
        app.config.bind_address = "127.0.0.1";
        app_get(&app, "/ping", ping_handler);
        app_listen(&app, port);
        app_destroy(&app);
        exit(0);
    }
    int up = 0;
    for (int i = 0; i < 50 && !up; i++) {
        up = get_ping("127.0.0.1", port);
        if (!up) nanosleep(&(struct timespec){0, 20 * 1000 * 1000}, NULL);
    }
    assert(up);
    char other[INET_ADDRSTRLEN];
    if (non_loopback_ipv4(other)) {
        assert(!get_ping(other, port));
    }
    assert(kill(pid, SIGTERM) == 0);
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* a bind address that isn't a numeric literal is a startup error on both paths: exit non-zero at once. */
static void test_app_listen_rejects_bad_bind_address(const int workers) {
    const pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        App app;
        app_init(&app);
        app.config.workers = workers;
        app.config.bind_address = "localhost";
        app_listen(&app, free_port());
        app_destroy(&app);
        exit(0);
    }
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) != 0);
}

int main(void) {
    test_app_init_defaults_to_every_ipv4_interface();
    test_null_binds_every_ipv4_interface();
    test_loopback_bind_refuses_other_interfaces();
    test_ipv6_addresses();
    test_non_numeric_addresses_rejected();
    test_app_listen_uses_bind_address(1);
    test_app_listen_uses_bind_address(2);
    test_app_listen_rejects_bad_bind_address(1);
    test_app_listen_rejects_bad_bind_address(2);
    printf("all listen tests passed\n");
    return 0;
}
