#ifndef TLS_H
#define TLS_H

#include <sys/types.h>
#include "app_types.h"

/*
 * Returns 1 if CExpress was compiled with TLS support enabled (CEXPRESS_HAS_TLS=1),
 * or 0 if compiled in plaintext-only mode.
 */
int tls_is_available(void);

/*
 * Initializes OpenSSL context for the application using app->config.tls_cert_file
 * and app->config.tls_key_file. Enforces TLS 1.2+ minimum, disables obsolete SSL/TLS
 * protocols, enables partial write mode, and verifies key matches certificate.
 * Returns 0 on success, -1 on failure.
 */
int tls_init_app(App *app);

/*
 * Tears down and frees app->ssl_ctx. Safe and idempotent.
 */
void tls_cleanup_app(App *app);

/*
 * Allocates and binds a new SSL session for a newly accepted connection.
 * Puts the SSL session into accept state and sets conn->tls_state = TLS_STATE_HANDSHAKE.
 * Returns 0 on success, -1 on failure.
 */
int tls_connection_init(App *app, Connection *conn);

/*
 * Performs a non-blocking TLS handshake step on conn->ssl.
 * Coordinates with the event loop:
 * - If SSL_ERROR_WANT_READ: registers read interest, unregisters write.
 * - If SSL_ERROR_WANT_WRITE: registers write interest, unregisters read.
 * Returns 1 if handshake is complete (transitions to TLS_STATE_CONNECTED),
 *         0 if handshake is in progress (waiting on I/O readiness),
 *        -1 on fatal handshake error (untrusted/aborted/invalid client).
 */
int tls_connection_handshake(App *app, Connection *conn);

/*
 * Non-blocking read from conn->ssl.
 * Returns:
 *   > 0 : bytes read into buf
 *     0 : clean TLS shutdown (SSL_ERROR_ZERO_RETURN / close_notify)
 *    -1 : on error, with errno set:
 *         - EAGAIN / EWOULDBLOCK if waiting for socket I/O (SSL_ERROR_WANT_READ/WANT_WRITE)
 *         - EIO on fatal SSL protocol error
 */
ssize_t tls_connection_read(App *app, Connection *conn, void *buf, size_t count);

/*
 * Non-blocking write to conn->ssl.
 * Returns:
 *   > 0 : bytes written from buf
 *    -1 : on error, with errno set:
 *         - EAGAIN / EWOULDBLOCK if socket send buffer full or waiting for read
 *         - EIO on fatal SSL protocol error
 */
ssize_t tls_connection_write(App *app, Connection *conn, const void *buf, size_t count);

/*
 * Gracefully shuts down and frees the SSL session for conn.
 * Sets conn->ssl = NULL and conn->tls_state = TLS_STATE_NONE.
 */
void tls_connection_close(App *app, Connection *conn);

/*
 * Checks if OpenSSL has decrypted application data buffered internally in memory
 * (SSL_pending(ssl) > 0) that has not yet been read by the application.
 */
int tls_has_pending(const Connection *conn);

#endif /* TLS_H */
