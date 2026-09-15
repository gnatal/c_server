#ifndef APP_MIDDLEWARES_H
#define APP_MIDDLEWARES_H

#include "appTypes.h"

/* This application's own cap on request bodies, independent of and tighter
 * than the engine's hard per-connection buffer limit (BUF_SIZE). */
#define MAX_APP_BODY_SIZE 4096

/* Logs "METHOD PATH -> STATUS" once the rest of the pipeline has produced a
 * response (morgan-style access log). */
void mw_logger(const Request *req, Response *res, MiddlewareChain *chain);

/* Rejects any request whose declared Content-Length exceeds
 * MAX_APP_BODY_SIZE with a 400 via chain_error(), before it reaches a
 * handler. */
void mw_body_size_guard(const Request *req, Response *res, MiddlewareChain *chain);

/* App-wide error handler: formats whatever chain_error() was called with as
 * a JSON body instead of the default plain-text one. */
void error_handler_json(int status, const char *message, const Request *req, Response *res);

/* Rejects any request whose "Authorization: Bearer <token>" header is
 * missing, malformed, or whose token doesn't match this app's configured
 * API key, with a 401 via chain_error(), before it reaches a handler. */
void mw_authenticate(const Request *req, Response *res, MiddlewareChain *chain);

#endif /* APP_MIDDLEWARES_H */
