#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include "app_types.h"
#include "event_loop.h"
#include "connection.h"
#include "router.h"
#include "response.h"
#include "middleware.h"
#include "tls.h"

#if CEXPRESS_HAS_TLS
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

static const char *TEST_CERT_FILE = "tests/certs/server.crt";
static const char *TEST_KEY_FILE = "tests/certs/server.key";

#if CEXPRESS_HAS_TLS
static void ping_handler(const Request *req, Response *res) {
    (void)req;
    res_status(res, 200);
    res_send(res, "pong");
}

static void echo_handler(const Request *req, Response *res) {
    res_status(res, 200);
    res_send(res, req->body ? req->body : "");
}

static void stream_handler(const Request *req, Response *res) {
    (void)req;
    res_status(res, 200);
    res_write(res, "first chunk\n", 12);
    res_write(res, "second chunk\n", 13);
    res_end(res);
}
#endif

static void test_tls_availability_and_config(void) {
#if CEXPRESS_HAS_TLS
    assert(tls_is_available() == 1);
#else
    assert(tls_is_available() == 0);
#endif

    App app;
    app_init(&app);
    assert(app.config.tls_enabled == 0);
    assert(app.ssl_ctx == NULL);

    assert(app_enable_tls(&app, TEST_CERT_FILE, TEST_KEY_FILE) == 0);
    assert(app.config.tls_enabled == 1);
    assert(strcmp(app.config.tls_cert_file, TEST_CERT_FILE) == 0);
    assert(strcmp(app.config.tls_key_file, TEST_KEY_FILE) == 0);

    /* Invalid arguments */
    assert(app_enable_tls(NULL, TEST_CERT_FILE, TEST_KEY_FILE) == -1);
    assert(app_enable_tls(&app, "", TEST_KEY_FILE) == -1);
    assert(app_enable_tls(&app, TEST_CERT_FILE, "") == -1);

    app_destroy(&app);
}

#if CEXPRESS_HAS_TLS

static void test_tls_init_app_and_cleanup(void) {
    App app;
    app_init(&app);
    assert(app_enable_tls(&app, TEST_CERT_FILE, TEST_KEY_FILE) == 0);
    assert(tls_init_app(&app) == 0);
    assert(app.ssl_ctx != NULL);

    /* Cleanup is safe and idempotent */
    tls_cleanup_app(&app);
    assert(app.ssl_ctx == NULL);
    tls_cleanup_app(&app);
    assert(app.ssl_ctx == NULL);

    app_destroy(&app);
}

static void test_tls_init_app_missing_files(void) {
    App app;
    app_init(&app);
    assert(app_enable_tls(&app, "tests/certs/nonexistent.crt", TEST_KEY_FILE) == 0);
    assert(tls_init_app(&app) != 0);
    assert(app.ssl_ctx == NULL);
    app_destroy(&app);

    app_init(&app);
    assert(app_enable_tls(&app, TEST_CERT_FILE, "tests/certs/nonexistent.key") == 0);
    assert(tls_init_app(&app) != 0);
    assert(app.ssl_ctx == NULL);
    app_destroy(&app);
}

/* Helper to run non-blocking handshake across socketpair */
static void drive_handshake(App *app, Connection *conn, SSL *c_ssl) {
    int client_done = 0;
    int server_done = 0;
    int iterations = 0;

    while ((!client_done || !server_done) && iterations++ < 100) {
        if (!client_done) {
            int r = SSL_do_handshake(c_ssl);
            if (r == 1) {
                client_done = 1;
            } else {
                int err = SSL_get_error(c_ssl, r);
                assert(err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE);
            }
        }

        if (!server_done) {
            int r = tls_connection_handshake(app, conn);
            if (r == 1) {
                server_done = 1;
            } else if (r == 0) {
                /* Handshake step succeeded, awaiting further socket events */
            } else {
                assert(0 && "server handshake failed unexpectedly");
            }
        }
    }

    assert(client_done == 1);
    assert(server_done == 1);
    assert(conn->tls_state == TLS_STATE_CONNECTED);
}

static void setup_test_tls_pair(App *app, int fds[2], Connection **conn, SSL_CTX **c_ctx, SSL **c_ssl) {
    app_init(app);
    assert(app_enable_tls(app, TEST_CERT_FILE, TEST_KEY_FILE) == 0);
    assert(tls_init_app(app) == 0);
    assert(event_loop_init(app) == 0);

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    assert(set_nonblocking(fds[0]) == 0);
    assert(set_nonblocking(fds[1]) == 0);

    *conn = connection_create(fds[0]);
    assert(*conn != NULL);
    app->connections[fds[0]] = *conn;

    assert(tls_connection_init(app, *conn) == 0);
    assert((*conn)->tls_state == TLS_STATE_HANDSHAKE);

    /* Setup client SSL context */
    *c_ctx = SSL_CTX_new(TLS_client_method());
    assert(*c_ctx != NULL);
    SSL_CTX_set_verify(*c_ctx, SSL_VERIFY_NONE, NULL);

    *c_ssl = SSL_new(*c_ctx);
    assert(*c_ssl != NULL);
    assert(SSL_set_fd(*c_ssl, fds[1]) > 0);
    SSL_set_connect_state(*c_ssl);

    drive_handshake(app, *conn, *c_ssl);
}

static void teardown_test_tls_pair(App *app, int fds[2], SSL_CTX *c_ctx, SSL *c_ssl) {
    if (c_ssl != NULL) {
        SSL_shutdown(c_ssl);
        SSL_free(c_ssl);
    }
    if (c_ctx != NULL) {
        SSL_CTX_free(c_ctx);
    }
    close(fds[1]);
    app_destroy(app);
}

static void test_tls_request_response_round_trip(void) {
    App app;
    int fds[2];
    Connection *conn;
    SSL_CTX *c_ctx;
    SSL *c_ssl;

    setup_test_tls_pair(&app, fds, &conn, &c_ctx, &c_ssl);
    app_get(&app, "/ping", ping_handler);

    const char *req = "GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\n";
    int nw = SSL_write(c_ssl, req, (int)strlen(req));
    assert(nw == (int)strlen(req));

    handle_readable(&app, conn);

    char resp[1024];
    memset(resp, 0, sizeof(resp));
    int nr = SSL_read(c_ssl, resp, sizeof(resp) - 1);
    assert(nr > 0);
    assert(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    assert(strstr(resp, "pong") != NULL);
    assert(conn->keep_alive == 1);

    teardown_test_tls_pair(&app, fds, c_ctx, c_ssl);
}

static void test_tls_keep_alive_multiple_requests(void) {
    App app;
    int fds[2];
    Connection *conn;
    SSL_CTX *c_ctx;
    SSL *c_ssl;

    setup_test_tls_pair(&app, fds, &conn, &c_ctx, &c_ssl);
    app_get(&app, "/ping", ping_handler);
    app_post(&app, "/echo", echo_handler);

    /* Request 1: GET /ping */
    const char *req1 = "GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\n";
    int nw = SSL_write(c_ssl, req1, (int)strlen(req1));
    assert(nw == (int)strlen(req1));

    handle_readable(&app, conn);

    char resp1[1024];
    memset(resp1, 0, sizeof(resp1));
    int nr = SSL_read(c_ssl, resp1, sizeof(resp1) - 1);
    assert(nr > 0);
    assert(strstr(resp1, "pong") != NULL);
    assert(conn->keep_alive == 1);

    /* Request 2: POST /echo over the same TLS connection */
    const char *req2 = "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 11\r\n\r\nhello-tls-2";
    nw = SSL_write(c_ssl, req2, (int)strlen(req2));
    assert(nw == (int)strlen(req2));

    handle_readable(&app, conn);

    char resp2[1024];
    memset(resp2, 0, sizeof(resp2));
    nr = SSL_read(c_ssl, resp2, sizeof(resp2) - 1);
    assert(nr > 0);
    assert(strstr(resp2, "hello-tls-2") != NULL);
    assert(conn->keep_alive == 1);

    teardown_test_tls_pair(&app, fds, c_ctx, c_ssl);
}

static void echo_len_handler(const Request *req, Response *res) {
    char body[64];
    snprintf(body, sizeof(body), "received %d bytes", req->content_length);
    res_status(res, 200);
    res_send(res, body);
}

static void large_resp_handler(const Request *req, Response *res) {
    (void)req;
    const size_t size = 32 * 1024;
    unsigned char *buf = malloc(size);
    assert(buf != NULL);
    memset(buf, 'Z', size);
    res_status(res, 200);
    res_send_bytes(res, "application/octet-stream", buf, size);
    free(buf);
}

static void test_tls_post_large_body(void) {
    App app;
    int fds[2];
    Connection *conn;
    SSL_CTX *c_ctx;
    SSL *c_ssl;

    setup_test_tls_pair(&app, fds, &conn, &c_ctx, &c_ssl);
    app_post(&app, "/echo", echo_len_handler);

    const size_t body_size = 20000;
    char *large_body = malloc(body_size + 1);
    assert(large_body != NULL);
    memset(large_body, 'A', body_size);
    large_body[body_size] = '\0';

    char header[256];
    int hlen = snprintf(header, sizeof(header),
                        "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: %zu\r\n\r\n", body_size);

    int nw = SSL_write(c_ssl, header, hlen);
    assert(nw == hlen);
    handle_readable(&app, conn);

    /* Stream body in chunks and call handle_readable so the non-blocking socket buffer doesn't overflow */
    int saw_growth = 0;
    size_t sent = 0;
    while (sent < body_size) {
        size_t remaining = body_size - sent;
        size_t chunk = remaining < 4096 ? remaining : 4096;
        nw = SSL_write(c_ssl, large_body + sent, (int)chunk);
        assert(nw > 0);
        sent += (size_t)nw;
        handle_readable(&app, conn);
        if (conn->in_cap > BUF_SIZE) {
            saw_growth = 1;
        }
    }
    assert(saw_growth);

    /* Read response header and echo length */
    char resp_buf[512];
    memset(resp_buf, 0, sizeof(resp_buf));
    int nr = SSL_read(c_ssl, resp_buf, sizeof(resp_buf) - 1);
    assert(nr > 0);
    assert(strstr(resp_buf, "HTTP/1.1 200 OK") != NULL);
    assert(strstr(resp_buf, "received 20000 bytes") != NULL);

    free(large_body);
    teardown_test_tls_pair(&app, fds, c_ctx, c_ssl);
}

static void test_tls_partial_write_large_response(void) {
    App app;
    int fds[2];
    Connection *conn;
    SSL_CTX *c_ctx;
    SSL *c_ssl;

    setup_test_tls_pair(&app, fds, &conn, &c_ctx, &c_ssl);
    app_get(&app, "/large", large_resp_handler);

    const char *req = "GET /large HTTP/1.1\r\nHost: localhost\r\n\r\n";
    int nw = SSL_write(c_ssl, req, (int)strlen(req));
    assert(nw == (int)strlen(req));

    handle_readable(&app, conn);

    /* Drain response in cooperative turns between SSL_read and flush_connection */
    const size_t expected_body = 32 * 1024;
    size_t recvd = 0;
    char *buf = malloc(expected_body + 4096);
    assert(buf != NULL);

    int loops = 0;
    char *bstart = NULL;
    while ((bstart == NULL || recvd < (size_t)(bstart + 4 - buf) + expected_body) && loops++ < 200) {
        if (conn->out_buf != NULL && conn->out_sent < conn->out_len) {
            flush_connection(&app, conn);
        }
        int nr = SSL_read(c_ssl, buf + recvd, (int)(expected_body + 4096 - recvd));
        if (nr > 0) {
            recvd += (size_t)nr;
            buf[recvd] = '\0';
            if (bstart == NULL) {
                bstart = strstr(buf, "\r\n\r\n");
            }
        }
    }

    assert(strstr(buf, "HTTP/1.1 200 OK") != NULL);
    assert(bstart != NULL);
    size_t body_recvd = recvd - (size_t)(bstart + 4 - buf);
    assert(body_recvd == expected_body);

    free(buf);
    teardown_test_tls_pair(&app, fds, c_ctx, c_ssl);
}

static void test_tls_chunked_streaming_response(void) {
    App app;
    int fds[2];
    Connection *conn;
    SSL_CTX *c_ctx;
    SSL *c_ssl;

    setup_test_tls_pair(&app, fds, &conn, &c_ctx, &c_ssl);
    app_get(&app, "/stream", stream_handler);

    const char *req = "GET /stream HTTP/1.1\r\nHost: localhost\r\n\r\n";
    int nw = SSL_write(c_ssl, req, (int)strlen(req));
    assert(nw == (int)strlen(req));

    handle_readable(&app, conn);

    char resp[1024];
    memset(resp, 0, sizeof(resp));
    int nr = SSL_read(c_ssl, resp, sizeof(resp) - 1);
    assert(nr > 0);
    assert(strstr(resp, "Transfer-Encoding: chunked") != NULL);
    assert(strstr(resp, "first chunk") != NULL);
    assert(strstr(resp, "second chunk") != NULL);

    teardown_test_tls_pair(&app, fds, c_ctx, c_ssl);
}

static void test_tls_bad_client_handshake_rejected(void) {
    App app;
    app_init(&app);
    assert(app_enable_tls(&app, TEST_CERT_FILE, TEST_KEY_FILE) == 0);
    assert(tls_init_app(&app) == 0);
    assert(event_loop_init(&app) == 0);

    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    assert(set_nonblocking(fds[0]) == 0);
    assert(set_nonblocking(fds[1]) == 0);

    Connection *conn = connection_create(fds[0]);
    assert(conn != NULL);
    app.connections[fds[0]] = conn;
    assert(tls_connection_init(&app, conn) == 0);

    /* Client sends invalid data instead of TLS ClientHello */
    const char *garbage = "NOT A VALID TLS HANDSHAKE PACKET\r\n\r\n";
    ssize_t w = write(fds[1], garbage, strlen(garbage));
    assert(w == (ssize_t)strlen(garbage));

    int hs = tls_connection_handshake(&app, conn);
    assert(hs == -1); /* Handshake rejected */

    close(fds[1]);
    app_destroy(&app);
}

static void test_tls_clean_shutdown(void) {
    App app;
    int fds[2];
    Connection *conn;
    SSL_CTX *c_ctx;
    SSL *c_ssl;

    setup_test_tls_pair(&app, fds, &conn, &c_ctx, &c_ssl);

    /* Client sends close_notify shutdown */
    SSL_shutdown(c_ssl);

    /* Server reading from cleanly closed SSL sees EOF (0) */
    char buf[128];
    ssize_t n = tls_connection_read(&app, conn, buf, sizeof(buf));
    assert(n == 0);

    teardown_test_tls_pair(&app, fds, c_ctx, c_ssl);
}

#endif /* CEXPRESS_HAS_TLS */

int main(void) {
    test_tls_availability_and_config();

#if CEXPRESS_HAS_TLS
    test_tls_init_app_and_cleanup();
    test_tls_init_app_missing_files();
    test_tls_request_response_round_trip();
    test_tls_keep_alive_multiple_requests();
    test_tls_post_large_body();
    test_tls_partial_write_large_response();
    test_tls_chunked_streaming_response();
    test_tls_bad_client_handshake_rejected();
    test_tls_clean_shutdown();
#endif

    printf("all tls tests passed\n");
    return 0;
}
