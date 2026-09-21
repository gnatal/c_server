#ifndef ROUTER_H
#define ROUTER_H

#include "app_types.h"

/*
 * Route registration. Deny by default: a request with no matching route gets 404 (path unknown) or
 * 405 + Allow (path known, other method), never a silent fallback. Register everything before app_listen.
 *
 * Pattern syntax (segments split on '/', empty segments ignored):
 *   "/users"          literal segments compare exactly
 *   "/users/:id"      ':name' captures one segment into req_get_param (value truncated to 63 chars)
 *   a '*' segment     in the middle of a pattern (users, then '*', then edit) matches exactly one
 *                     segment and captures nothing
 *   trailing '*'      as the LAST segment (files, then '*') matches one or more remaining segments;
 *                     the bare prefix ("/files") does not match
 * Matching walks a per-method tree of path segments: a literal segment beats ':name', which beats '*',
 * whatever the registration order ("/users/me" wins over "/users/:id"). A second registration of the same pattern
 * is ignored with a warning. Use the same ':name' at the same position in every route: the capture is stored under
 * the name of the first route registered there (lib/CLAUDE.md, "Known gaps"). HEAD falls back to the GET route for
 * the same path unless an explicit HEAD route exists. OPTIONS on a known path answers 200 + Allow without an explicit route.
 * Limits: dynamically allocated routes per App, MAX_ROUTER_ROUTES (64) per Router, MAX_ROUTE_MIDDLEWARES (8) per route; excess is dropped with
 * a stderr warning, not overflowed. Strings are copied, the caller's buffers need not outlive the call.
 */

/* Resets an App: zero routes/middleware, no error handler, no TLS, allocates the connection table. Pair with app_destroy. */
void app_init(App *app);

/* Generic form; every helper below is this with a fixed method. `method` is upper-case ("GET"). */
void app_add_route(App *app, const char *method, const char *path, Handler handler);

/*
 * Same, with per-route middleware: run after all app-wide middleware and before `handler`, only for
 * this route. `middlewares` may be NULL when count is 0. Idiom:
 *   app_post_mw(app, "/items", handler, (Middleware[]){ mw_auth, mw_rate_limit }, 2);
 */
void app_add_route_mw(App *app, const char *method, const char *path, Handler handler,
                      const Middleware *middlewares, int middleware_count);

/* Method helpers: app_get(app, "/users/:id", handler), and so on. */
void app_get(App *app, const char *path, Handler handler);
void app_post(App *app, const char *path, Handler handler);
void app_put(App *app, const char *path, Handler handler);
void app_patch(App *app, const char *path, Handler handler);
void app_delete(App *app, const char *path, Handler handler);
void app_head(App *app, const char *path, Handler handler);    /* overrides the automatic HEAD-from-GET */
void app_options(App *app, const char *path, Handler handler); /* overrides the automatic OPTIONS answer */

/* Method helpers with per-route middleware (see app_add_route_mw). */
void app_get_mw(App *app, const char *path, Handler handler, const Middleware *middlewares, int middleware_count);
void app_post_mw(App *app, const char *path, Handler handler, const Middleware *middlewares, int middleware_count);
void app_put_mw(App *app, const char *path, Handler handler, const Middleware *middlewares, int middleware_count);
void app_patch_mw(App *app, const char *path, Handler handler, const Middleware *middlewares, int middleware_count);
void app_delete_mw(App *app, const char *path, Handler handler, const Middleware *middlewares, int middleware_count);
void app_head_mw(App *app, const char *path, Handler handler, const Middleware *middlewares, int middleware_count);
void app_options_mw(App *app, const char *path, Handler handler, const Middleware *middlewares, int middleware_count);

/*
 * Route matching (called by the engine; exposed for tests).
 * match_path: 1 if `path` matches `pattern`. Captures ':name' segments into req (param_count is reset first);
 *   req may be NULL to test a match without capturing. Never writes outside req's param slots.
 * match_route: first route matching req->method and req->path (with the HEAD->GET fallback), or NULL.
 *   Fills req's path params for the matched route.
 * match_route_allowed_methods: comma-separated, de-duplicated methods of every route matching
 *   req->path, in registration order, into `allowed`; returns how many (0 = a true 404). Does not modify req.
 * req_get_param: captured value (percent-decoded, first match) or NULL.
 */
int match_path(const char *pattern, const char *path, Request *req);
const Route *match_route(const App *app, Request *req);
int match_route_allowed_methods(const App *app, const Request *req, char *allowed, size_t allowed_size);
const char *req_get_param(const Request *req, const char *name);

/*
 * Sub-routers (Express Router). Build a Router with the router_* twins of the app_* functions, then
 * app_mount(app, "/api", &router): routes are COPIED into app->routes with the prefix prepended
 * (a router route "/" mounts at "/api", not "/api/"), and router_use middleware becomes app-wide
 * middleware scoped to the prefix. After app_mount the Router may go out of scope. Prefix "" or "/"
 * mounts unscoped; a trailing '/' in the prefix is stripped. Routers do not nest.
 */
void router_init(Router *router);
void router_add_route(Router *router, const char *method, const char *path, Handler handler);
void router_add_route_mw(Router *router, const char *method, const char *path, Handler handler,
                         const Middleware *middlewares, int middleware_count);
void router_get(Router *router, const char *path, Handler handler);
void router_post(Router *router, const char *path, Handler handler);
void router_put(Router *router, const char *path, Handler handler);
void router_patch(Router *router, const char *path, Handler handler);
void router_delete(Router *router, const char *path, Handler handler);
void router_head(Router *router, const char *path, Handler handler);
void router_options(Router *router, const char *path, Handler handler);
void router_get_mw(Router *router, const char *path, Handler handler, const Middleware *middlewares, int middleware_count);
void router_post_mw(Router *router, const char *path, Handler handler, const Middleware *middlewares, int middleware_count);
void router_put_mw(Router *router, const char *path, Handler handler, const Middleware *middlewares, int middleware_count);
void router_patch_mw(Router *router, const char *path, Handler handler, const Middleware *middlewares, int middleware_count);
void router_delete_mw(Router *router, const char *path, Handler handler, const Middleware *middlewares, int middleware_count);
void router_head_mw(Router *router, const char *path, Handler handler, const Middleware *middlewares, int middleware_count);
void router_options_mw(Router *router, const char *path, Handler handler, const Middleware *middlewares, int middleware_count);
void router_use(Router *router, Middleware mw); /* router-level middleware; takes effect at app_mount */
void app_mount(App *app, const char *prefix, const Router *router);

/*
 * Static files: GET <prefix>/... is served from root_dir (Express express.static). root_dir is
 * realpath()'d at registration; a missing directory registers nothing (logged). Requests are refused
 * with 403 if they contain a ".." segment or resolve (symlinks included) outside root_dir, 404 if not
 * a regular file; a directory serves its index.html; there is never a directory listing. Files over
 * MAX_STATIC_FILE_SIZE (50 MiB) get 500. Counts as one route. App-level only (no router twin).
 */
void app_serve_static(App *app, const char *prefix, const char *root_dir);

/* Enables HTTPS with PEM files (TLS 1.2+). Returns 0, or -1 on bad arguments / path >= PATH_MAX.
 * The context is created per worker process at app_listen. */
int app_enable_tls(App *app, const char *cert_file, const char *key_file);

/* Frees dynamically allocated route tree memory. Called by app_destroy. */
void app_free_routes(App *app);

#endif /* ROUTER_H */
