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
 * Express-style convenience wrapper: app_head(app, "/users", handler).
 * Registering an explicit HEAD route always takes priority over the
 * auto-HEAD-from-GET fallback in match_route (router.c) for the same path.
 */
void app_head(App *app, const char *path, Handler handler);

/*
 * Express-style convenience wrapper: app_options(app, "/users", handler).
 * Registering an explicit OPTIONS route always takes priority over the
 * auto-OPTIONS fallback in dispatch/chain_next (middleware.c) for the same
 * path.
 */
void app_options(App *app, const char *path, Handler handler);

/*
 * Express-style convenience wrapper with per-route middleware:
 * app_head_mw(app, "/users", handler, (Middleware[]){mw1, mw2}, 2).
 */
void app_head_mw(App *app, const char *path, Handler handler,
                  const Middleware *middlewares, int middleware_count);

/*
 * Express-style convenience wrapper with per-route middleware:
 * app_options_mw(app, "/users", handler, (Middleware[]){mw1, mw2}, 2).
 */
void app_options_mw(App *app, const char *path, Handler handler,
                     const Middleware *middlewares, int middleware_count);

/*
 * Compares a route pattern (e.g. "/users/:id") against an actual request
 * path segment by segment. Literal segments must match exactly; segments
 * starting with ':' capture the corresponding path segment into req->params.
 * A "*" segment is a wildcard: mid-pattern (e.g. "/users/" followed by a "*"
 * segment then "/edit") it matches exactly one path segment without
 * capturing it; as the pattern's last segment (e.g. "/files/" followed by a
 * trailing "*") it matches that segment and everything after it, so that
 * pattern matches "/files/a" and "/files/a/b/c" but not "/files" itself
 * (no trailing segment to match against).
 */
int match_path(const char *pattern, const char *path, Request *req);

/*
 * Finds the first registered route whose method and path pattern match the
 * request. If req->method is "HEAD" and no route matches under "HEAD"
 * itself, falls back to the first route matching the same path under "GET"
 * (auto-HEAD-from-GET, mirroring Express - app_get() implicitly makes HEAD
 * work too, unless an explicit app_head() route for that path exists, which
 * always wins since it's tried first).
 */
const Route *match_route(const App *app, Request *req);

/*
 * Scans every registered route for one whose path pattern matches req->path,
 * regardless of method, and writes a deduplicated, comma-separated list of
 * their methods (in route-registration order) into allowed. Used to tell a
 * true 404 (no route matches this path at all) apart from a 405 (path
 * matches, just not with this method) once match_route has already returned
 * NULL. Does not mutate req - match_path's param-capture writes go into a
 * scratch copy. Returns how many distinct methods were found (0 means no
 * route matches this path, i.e. a real 404).
 */
int match_route_allowed_methods(const App *app, const Request *req, char *allowed, size_t allowed_size);

/* Looks up a captured path param by name (e.g. req_get_param(req, "id")).
 * Captured values are URL-decoded, since match_path always runs against
 * req->path, which parse_http_request already decoded (http_parser.c) before
 * routing ever sees it - match_path itself does no decoding of its own. */
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

/* Express-style convenience wrapper: router_head(router, "/users", handler). */
void router_head(Router *router, const char *path, Handler handler);

/* Express-style convenience wrapper: router_options(router, "/users", handler). */
void router_options(Router *router, const char *path, Handler handler);

/*
 * Express-style convenience wrapper with per-route middleware:
 * router_head_mw(router, "/users", handler, (Middleware[]){mw1, mw2}, 2).
 */
void router_head_mw(Router *router, const char *path, Handler handler,
                     const Middleware *middlewares, int middleware_count);

/*
 * Express-style convenience wrapper with per-route middleware:
 * router_options_mw(router, "/users", handler, (Middleware[]){mw1, mw2}, 2).
 */
void router_options_mw(Router *router, const char *path, Handler handler,
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

/*
 * Registers a static-file mount at a path prefix - the C analogue of
 * Express's app.use('/static', express.static('root_dir')). Any GET request
 * under prefix (e.g. "/static/js/app.js") is answered by reading the
 * corresponding file out of root_dir (lib/static.h: static_serve_file),
 * instead of calling a Handler - there is no Handler to register here.
 * root_dir is resolved to an absolute, canonical path via realpath() at
 * registration time; if it doesn't exist, this logs a warning and does not
 * register anything (deny-by-default, same as every other hard failure in
 * this engine's registration functions). Subject to the same MAX_ROUTES cap
 * as any other route. App-level only - there is no router_serve_static/
 * app_mount equivalent yet. See lib/CLAUDE.md ("Static file serving").
 */
void app_serve_static(App *app, const char *prefix, const char *root_dir);

/*
 * Configures TLS certificate and private key paths for the application.
 * Returns 0 on success, -1 on failure (e.g. invalid arguments or path exceeding PATH_MAX).
 */
int app_enable_tls(App *app, const char *cert_file, const char *key_file);

#endif /* ROUTER_H */
