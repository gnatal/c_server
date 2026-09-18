#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include "tls.h"
#include "event_loop.h"
#include "connection.h"

#if CEXPRESS_HAS_TLS

#include <openssl/ssl.h>
#include <openssl/err.h>

int tls_is_available(void) {
    return 1;
}

int tls_init_app(App *app) {
    if (app == NULL || !app->config.tls_enabled) {
        return 0;
    }

    if (app->config.tls_cert_file[0] == '\0' || app->config.tls_key_file[0] == '\0') {
        fprintf(stderr, "tls_init_app: both certificate and key file must be configured\n");
        return -1;
    }

    /* Ignore SIGPIPE so writing to an abruptly closed SSL socket does not
     * kill the process - connection.c: app_listen_worker now does this
     * unconditionally for the full server (see lib/CLAUDE.md, "SIGPIPE
     * handling"), but this call still matters in its own right: it's what
     * protects any caller that exercises real TLS socket teardown without
     * going through app_listen_worker, e.g. tests/test_tls.c. signal() is
     * idempotent, so calling it again here is harmless. */
    signal(SIGPIPE, SIG_IGN);

    const SSL_METHOD *method = TLS_server_method();
    SSL_CTX *ctx = SSL_CTX_new(method);
    if (ctx == NULL) {
        fprintf(stderr, "tls_init_app: SSL_CTX_new failed\n");
        ERR_print_errors_fp(stderr);
        return -1;
    }

    /* Modern security posture: enforce TLS 1.2 minimum, disable legacy SSLv2/v3 and TLS 1.0/1.1 */
#if defined(TLS1_2_VERSION)
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
#endif
    SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1 |
                                 SSL_OP_NO_TLSv1_1 | SSL_OP_CIPHER_SERVER_PREFERENCE);

    /* Allow partial writes and moving write buffers for non-blocking flush loops */
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    /* Load certificate chain */
    if (SSL_CTX_use_certificate_chain_file(ctx, app->config.tls_cert_file) <= 0) {
        fprintf(stderr, "tls_init_app: failed to load certificate chain from '%s'\n",
                app->config.tls_cert_file);
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx);
        return -1;
    }

    /* Load private key */
    if (SSL_CTX_use_PrivateKey_file(ctx, app->config.tls_key_file, SSL_FILETYPE_PEM) <= 0) {
        fprintf(stderr, "tls_init_app: failed to load private key from '%s'\n",
                app->config.tls_key_file);
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx);
        return -1;
    }

    /* Verify that private key matches the public certificate */
    if (!SSL_CTX_check_private_key(ctx)) {
        fprintf(stderr, "tls_init_app: private key does not match public certificate\n");
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx);
        return -1;
    }

    app->ssl_ctx = ctx;
    return 0;
}

void tls_cleanup_app(App *app) {
    if (app != NULL && app->ssl_ctx != NULL) {
        SSL_CTX_free((SSL_CTX *)app->ssl_ctx);
        app->ssl_ctx = NULL;
    }
}

int tls_connection_init(App *app, Connection *conn) {
    if (app == NULL || app->ssl_ctx == NULL || conn == NULL) {
        return -1;
    }

    SSL *ssl = SSL_new((SSL_CTX *)app->ssl_ctx);
    if (ssl == NULL) {
        return -1;
    }

    if (SSL_set_fd(ssl, conn->fd) <= 0) {
        SSL_free(ssl);
        return -1;
    }

    SSL_set_accept_state(ssl);
    conn->ssl = ssl;
    conn->tls_state = TLS_STATE_HANDSHAKE;
    conn->tls_want_read = 0;
    conn->tls_want_write = 0;
    return 0;
}

int tls_connection_handshake(App *app, Connection *conn) {
    if (app == NULL || conn == NULL || conn->ssl == NULL) {
        return -1;
    }

    SSL *ssl = (SSL *)conn->ssl;
    int ret = SSL_do_handshake(ssl);
    if (ret == 1) {
        conn->tls_state = TLS_STATE_CONNECTED;
        conn->tls_want_read = 0;
        conn->tls_want_write = 0;
        event_loop_unwatch_write(app, conn->fd, conn);
        event_loop_watch_read(app, conn->fd, conn);
        return 1;
    }

    int err = SSL_get_error(ssl, ret);
    if (err == SSL_ERROR_WANT_READ) {
        conn->tls_want_read = 1;
        conn->tls_want_write = 0;
        event_loop_unwatch_write(app, conn->fd, conn);
        event_loop_watch_read(app, conn->fd, conn);
        return 0;
    } else if (err == SSL_ERROR_WANT_WRITE) {
        conn->tls_want_write = 1;
        conn->tls_want_read = 0;
        event_loop_unwatch_read(app, conn->fd);
        event_loop_watch_write(app, conn->fd, conn);
        return 0;
    }

    /* Fatal handshake error: certificate rejection, client abort, protocol mismatch */
    return -1;
}

ssize_t tls_connection_read(App *app, Connection *conn, void *buf, size_t count) {
    if (app == NULL || conn == NULL || conn->ssl == NULL) {
        errno = EINVAL;
        return -1;
    }

    SSL *ssl = (SSL *)conn->ssl;
    int ret = SSL_read(ssl, buf, (int)count);
    if (ret > 0) {
        conn->last_activity = time(NULL);
        if (conn->tls_want_write) {
            conn->tls_want_write = 0;
            event_loop_unwatch_write(app, conn->fd, conn);
        }
        return (ssize_t)ret;
    }

    int err = SSL_get_error(ssl, ret);
    if (err == SSL_ERROR_WANT_READ) {
        conn->tls_want_read = 1;
        errno = EAGAIN;
        return -1;
    } else if (err == SSL_ERROR_WANT_WRITE) {
        conn->tls_want_write = 1;
        event_loop_watch_write(app, conn->fd, conn);
        errno = EAGAIN;
        return -1;
    } else if (err == SSL_ERROR_ZERO_RETURN) {
        /* Clean TLS shutdown notify from peer */
        return 0;
    } else if (err == SSL_ERROR_SYSCALL) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -1;
        }
        if (ret == 0) {
            return 0; /* Peer cleanly closed socket without close_notify */
        }
        errno = ECONNRESET;
        return -1;
    }

    errno = EIO;
    return -1;
}

ssize_t tls_connection_write(App *app, Connection *conn, const void *buf, size_t count) {
    if (app == NULL || conn == NULL || conn->ssl == NULL) {
        errno = EINVAL;
        return -1;
    }

    SSL *ssl = (SSL *)conn->ssl;
    int ret = SSL_write(ssl, buf, (int)count);
    if (ret > 0) {
        conn->last_activity = time(NULL);
        if (conn->tls_want_read) {
            conn->tls_want_read = 0;
            event_loop_unwatch_read(app, conn->fd);
        }
        return (ssize_t)ret;
    }

    int err = SSL_get_error(ssl, ret);
    if (err == SSL_ERROR_WANT_WRITE) {
        conn->tls_want_write = 1;
        errno = EAGAIN;
        return -1;
    } else if (err == SSL_ERROR_WANT_READ) {
        conn->tls_want_read = 1;
        event_loop_watch_read(app, conn->fd, conn);
        errno = EAGAIN;
        return -1;
    } else if (err == SSL_ERROR_SYSCALL) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -1;
        }
        errno = EPIPE;
        return -1;
    }

    errno = EIO;
    return -1;
}

void tls_connection_close(App *app, Connection *conn) {
    (void)app;
    if (conn != NULL && conn->ssl != NULL) {
        SSL *ssl = (SSL *)conn->ssl;
        /* Non-blocking shutdown notification attempt */
        SSL_shutdown(ssl);
        SSL_free(ssl);
        conn->ssl = NULL;
        conn->tls_state = TLS_STATE_NONE;
        conn->tls_want_read = 0;
        conn->tls_want_write = 0;
    }
}

int tls_has_pending(const Connection *conn) {
    if (conn != NULL && conn->ssl != NULL) {
        return SSL_pending((const SSL *)conn->ssl) > 0;
    }
    return 0;
}

#else /* !CEXPRESS_HAS_TLS */

int tls_is_available(void) {
    return 0;
}

int tls_init_app(App *app) {
    if (app != NULL && app->config.tls_enabled) {
        fprintf(stderr, "tls_init_app: CExpress was compiled without TLS support (CEXPRESS_HAS_TLS=0)\n");
        return -1;
    }
    return 0;
}

void tls_cleanup_app(App *app) {
    (void)app;
}

int tls_connection_init(App *app, Connection *conn) {
    (void)app;
    (void)conn;
    return -1;
}

int tls_connection_handshake(App *app, Connection *conn) {
    (void)app;
    (void)conn;
    return -1;
}

ssize_t tls_connection_read(App *app, Connection *conn, void *buf, size_t count) {
    (void)app;
    (void)conn;
    (void)buf;
    (void)count;
    errno = ENOSYS;
    return -1;
}

ssize_t tls_connection_write(App *app, Connection *conn, const void *buf, size_t count) {
    (void)app;
    (void)conn;
    (void)buf;
    (void)count;
    errno = ENOSYS;
    return -1;
}

void tls_connection_close(App *app, Connection *conn) {
    (void)app;
    (void)conn;
}

int tls_has_pending(const Connection *conn) {
    (void)conn;
    return 0;
}

#endif /* CEXPRESS_HAS_TLS */
