#ifndef ROUTER_H
#define ROUTER_H

#include "app_types.h"

/* Resets an App to have zero registered routes and no open connections. */
void app_init(App *app);

/* Registers a handler for a given HTTP method + path pattern. */
void app_add_route(App *app, const char *method, const char *path, Handler handler);

/*
 * Registers a handler with its own per-route middleware, run in dispatch()
 * after the app-wide app_use() middleware and before this handler - unlike
 * app_use(), these only run for requests that match this route. middlewares
 * may be NULL if middleware_count is 0. A middleware_count above
 * MAX_ROUTE_MIDDLEWARES is truncated (with a warning) rather than
 * overflowing the route's fixed array.
 */
void app_add_route_mw(App *app, const char *method, const char *path, Handler handler,
                       const Middleware *middlewares, int middleware_count);

/* Express-style convenience wrapper: app_get(app, "/users", handler). */
void app_get(App *app, const char *path, Handler handler);

/* Express-style convenience wrapper: app_post(app, "/users", handler). */
void app_post(App *app, const char *path, Handler handler);

/*
 * Express-style convenience wrapper with per-route middleware:
 * app_get_mw(app, "/users", handler, (Middleware[]){mw1, mw2}, 2).
 */
void app_get_mw(App *app, const char *path, Handler handler,
                 const Middleware *middlewares, int middleware_count);

/*
 * Express-style convenience wrapper with per-route middleware:
 * app_post_mw(app, "/users", handler, (Middleware[]){mw1, mw2}, 2).
 */
void app_post_mw(App *app, const char *path, Handler handler,
                  const Middleware *middlewares, int middleware_count);

/*
 * Compares a route pattern (e.g. "/users/:id") against an actual request
 * path segment by segment. Literal segments must match exactly; segments
 * starting with ':' capture the corresponding path segment into req->params.
 */
int match_path(const char *pattern, const char *path, Request *req);

/* Finds the first registered route whose method and path pattern match the request. */
const Route *match_route(const App *app, Request *req);

/* Looks up a captured path param by name (e.g. req_get_param(req, "id")). */
const char *req_get_param(const Request *req, const char *name);

#endif /* ROUTER_H */
