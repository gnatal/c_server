#ifndef MIDDLEWARE_H
#define MIDDLEWARE_H

#include "app_types.h"

/*
 * Registers app-wide middleware, run in registration order before every
 * request is dispatched - including requests that end up falling through
 * to a 404. Unscoped: equivalent to app_use_prefix(app, "", mw).
 */
void app_use(App *app, Middleware mw);

/*
 * Registers app-wide middleware scoped to a path prefix (the C analogue of
 * Express's app.use('/api', mw)): it only runs for requests whose req->path
 * starts with prefix at a segment boundary (prefix "/api" matches "/api" and
 * "/api/foo", not "/apiary"). prefix "" or "/" behaves like app_use (runs on
 * every request). Still runs ahead of route dispatch and in registration
 * order relative to other app-wide middleware; a request that doesn't match
 * the prefix skips this middleware entirely (its chain_next is never
 * called for it).
 */
void app_use_prefix(App *app, const char *prefix, Middleware mw);

/*
 * Registers the app's single centralized error handler. Middleware signals
 * a failure via chain_error() instead of building the error response
 * itself; registering again overwrites the previous handler.
 */
void app_use_error(App *app, ErrorHandler handler);

/*
 * Advances the pipeline by one step: runs the next unrun middleware, or -
 * once all middleware have run - the matched route's handler, or (if no
 * route matched) a 404 if nothing registered matches req->path at all, an
 * auto-OPTIONS 200 + Allow header if the request method is OPTIONS and
 * req->path matches a registered route under some other method (no explicit
 * app_options() route needed), or a 405 + Allow header for any other method
 * in that same "path matches, method doesn't" situation. Middleware calls
 * this to continue the chain.
 */
void chain_next(MiddlewareChain *chain);

/*
 * Short-circuits the pipeline for an error: hands off to the app's
 * registered error handler, or - if none is registered - falls back to
 * res_status()+res_send() with the given status and message directly.
 */
void chain_error(MiddlewareChain *chain, int status, const char *message);

/*
 * Builds a fresh MiddlewareChain for one request (the app's middleware list
 * plus the already-matched route, which may be NULL) and runs it to
 * completion.
 */
void dispatch(App *app, const Route *route, const Request *req, Response *res);

#endif /* MIDDLEWARE_H */
