#ifndef MIDDLEWARE_H
#define MIDDLEWARE_H

#include "app_types.h"

/*
 * Registers app-wide middleware, run in registration order before every
 * request is dispatched - including requests that end up falling through
 * to a 404 (no path-scoping yet; every Middleware sees every request).
 */
void app_use(App *app, Middleware mw);

/*
 * Registers the app's single centralized error handler. Middleware signals
 * a failure via chain_error() instead of building the error response
 * itself; registering again overwrites the previous handler.
 */
void app_use_error(App *app, ErrorHandler handler);

/*
 * Advances the pipeline by one step: runs the next unrun middleware, or -
 * once all middleware have run - the matched route's handler, or a 404 if
 * no route matched. Middleware calls this to continue the chain.
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
