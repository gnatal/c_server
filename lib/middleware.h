#ifndef MIDDLEWARE_H
#define MIDDLEWARE_H

#include "app_types.h"

/*
 * Middleware: void mw(const Request *req, Response *res, MiddlewareChain *chain).
 * It must do exactly one of:
 *   - call chain_next(chain) to continue (code after that call runs once the rest of the
 *     pipeline, including the handler, has finished, so res->status is final there);
 *   - call chain_error(chain, status, message) to fail the request;
 *   - write a response itself (res_send ...) and NOT call chain_next.
 * Route handlers are the terminal node: they get no chain and cannot call chain_next/chain_error.
 *
 * Execution order for one request: app-wide middleware in registration order (only those whose
 * prefix matches req->path, and also for requests that will 404) -> the matched route's own
 * middleware -> the handler. If no route matched, the pipeline ends in the default 404/405/OPTIONS
 * answer instead of a handler.
 * Limits: MAX_MIDDLEWARES (16) app-wide; excess is dropped with a stderr warning.
 */

/* App-wide middleware for every request. Same as app_use_prefix(app, "", mw). */
void app_use(App *app, Middleware mw);

/* App-wide middleware only for paths under `prefix` at a segment boundary: "/api" matches "/api"
 * and "/api/x", not "/apiary". Prefix "" or "/" matches everything. The prefix is normalized
 * (path_normalize_prefix) and req->path is canonical, so "//api/x" is covered too. */
void app_use_prefix(App *app, const char *prefix, Middleware mw);

/* Sets the one app-wide error handler that receives every chain_error(). The last call wins.
 * Without one, chain_error sends `status` with `message` as a text/plain body. */
void app_use_error(App *app, ErrorHandler handler);

/* Continues the pipeline (see above). Call at most once per middleware invocation. */
void chain_next(MiddlewareChain *chain);

/* Ends the pipeline with an error via the app's error handler. Do not call chain_next afterwards. */
void chain_error(MiddlewareChain *chain, int status, const char *message);

/*
 * Runs the pipeline for one request: `route` is match_route's result (NULL for none).
 * With no route: 404 when no route matches the path, 405 + Allow when the path exists under other
 * methods, 200 + Allow for OPTIONS on such a path. A static-file route is served here too.
 */
void dispatch(App *app, const Route *route, const Request *req, Response *res);

#endif /* MIDDLEWARE_H */
