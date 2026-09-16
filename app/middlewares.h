#ifndef APP_MIDDLEWARES_H
#define APP_MIDDLEWARES_H

#include "app_types.h"

/* This application's own cap on request bodies, independent of and tighter
 * than the engine's hard per-connection body limit (MAX_BODY_SIZE, 1 MiB -
 * see app_types.h). A body between this and MAX_BODY_SIZE is still fully
 * received by the engine (lib/CLAUDE.md, "Body buffering") before this
 * middleware ever gets a chance to reject it - to reject earlier, cheaper,
 * this would need to be an engine-level policy rather than app middleware. */
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

/* Configures the secret API key used by mw_authenticate. If not explicitly set,
 * falls back to the API_KEY environment variable, or a default fallback key. */
void mw_authenticate_set_key(const char *key);
const char *mw_authenticate_get_key(void);

#endif /* APP_MIDDLEWARES_H */
