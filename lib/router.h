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

/* Express-style convenience wrapper: app_put(app, "/users/:id", handler). */
void app_put(App *app, const char *path, Handler handler);

/* Express-style convenience wrapper: app_patch(app, "/users/:id", handler). */
void app_patch(App *app, const char *path, Handler handler);

/* Express-style convenience wrapper: app_delete(app, "/users/:id", handler). */
void app_delete(App *app, const char *path, Handler handler);

/*
 * Express-style convenience wrapper with per-route middleware:
 * app_put_mw(app, "/users/:id", handler, (Middleware[]){mw1, mw2}, 2).
 */
void app_put_mw(App *app, const char *path, Handler handler,
                 const Middleware *middlewares, int middleware_count);

/*
 * Express-style convenience wrapper with per-route middleware:
 * app_patch_mw(app, "/users/:id", handler, (Middleware[]){mw1, mw2}, 2).
 */
void app_patch_mw(App *app, const char *path, Handler handler,
                   const Middleware *middlewares, int middleware_count);

/*
 * Express-style convenience wrapper with per-route middleware:
 * app_delete_mw(app, "/users/:id", handler, (Middleware[]){mw1, mw2}, 2).
 */
void app_delete_mw(App *app, const char *path, Handler handler,
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

/* Resets a Router to have zero registered routes and zero router-level
 * middleware. A Router is a standalone route table (Express's Router()) -
 * it does nothing until mounted into an App via app_mount(). */
void router_init(Router *router);

/* Registers a handler on a router for a given HTTP method + path pattern -
 * the Router analogue of app_add_route. Paths are relative to wherever the
 * router ends up mounted (e.g. "/users" becomes "/api/users" once mounted
 * at "/api"). */
void router_add_route(Router *router, const char *method, const char *path, Handler handler);

/* Router analogue of app_add_route_mw: registers a handler with its own
 * per-route middleware on the router. */
void router_add_route_mw(Router *router, const char *method, const char *path, Handler handler,
                          const Middleware *middlewares, int middleware_count);

/* Express-style convenience wrapper: router_get(router, "/users", handler). */
void router_get(Router *router, const char *path, Handler handler);

/* Express-style convenience wrapper: router_post(router, "/users", handler). */
void router_post(Router *router, const char *path, Handler handler);

/*
 * Express-style convenience wrapper with per-route middleware:
 * router_get_mw(router, "/users", handler, (Middleware[]){mw1, mw2}, 2).
 */
void router_get_mw(Router *router, const char *path, Handler handler,
                    const Middleware *middlewares, int middleware_count);

/*
 * Express-style convenience wrapper with per-route middleware:
 * router_post_mw(router, "/users", handler, (Middleware[]){mw1, mw2}, 2).
 */
void router_post_mw(Router *router, const char *path, Handler handler,
                     const Middleware *middlewares, int middleware_count);

/* Express-style convenience wrapper: router_put(router, "/users/:id", handler). */
void router_put(Router *router, const char *path, Handler handler);

/* Express-style convenience wrapper: router_patch(router, "/users/:id", handler). */
void router_patch(Router *router, const char *path, Handler handler);

/* Express-style convenience wrapper: router_delete(router, "/users/:id", handler). */
void router_delete(Router *router, const char *path, Handler handler);

/*
 * Express-style convenience wrapper with per-route middleware:
 * router_put_mw(router, "/users/:id", handler, (Middleware[]){mw1, mw2}, 2).
 */
void router_put_mw(Router *router, const char *path, Handler handler,
                    const Middleware *middlewares, int middleware_count);

/*
 * Express-style convenience wrapper with per-route middleware:
 * router_patch_mw(router, "/users/:id", handler, (Middleware[]){mw1, mw2}, 2).
 */
void router_patch_mw(Router *router, const char *path, Handler handler,
                      const Middleware *middlewares, int middleware_count);

/*
 * Express-style convenience wrapper with per-route middleware:
 * router_delete_mw(router, "/users/:id", handler, (Middleware[]){mw1, mw2}, 2).
 */
void router_delete_mw(Router *router, const char *path, Handler handler,
                       const Middleware *middlewares, int middleware_count);

/*
 * Registers router-level middleware (the Router analogue of app_use): once
 * the router is mounted via app_mount(app, prefix, router), this runs ahead
 * of any of the router's own routes for requests under that mount's prefix -
 * it has no effect before the router is mounted.
 */
void router_use(Router *router, Middleware mw);

/*
 * Mounts a router's routes and router-level middleware onto app at a path
 * prefix - the C analogue of Express's app.use('/api', router). Flattens
 * (copies) the router's routes into app->routes with prefix prepended to
 * each path (a route registered at "/" mounts at the prefix itself), and
 * registers the router's middleware app-wide via app_use_prefix scoped to
 * that same prefix, so it only runs for requests under the mount point,
 * ahead of route dispatch. prefix "" or "/" mounts unscoped (matches every
 * request), same as app_use_prefix. Both the router's routes and its
 * middleware are subject to the same MAX_ROUTES/MAX_MIDDLEWARES caps as any
 * other app_add_route_mw/app_use_prefix call, truncating with a stderr
 * warning rather than overflowing.
 */
void app_mount(App *app, const char *prefix, const Router *router);

#endif /* ROUTER_H */
