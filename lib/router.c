#include <stdio.h>
#include <string.h>
#include "router.h"
#include "middleware.h"

void app_init(App *app) {
    app->config.port = DEFAULT_PORT;
    app->route_count = 0;
    app->middleware_count = 0;
    app->error_handler = NULL;
    app->server_fd = -1;
    app->kq = -1;
    memset(app->connections, 0, sizeof(app->connections));
}

/* Shared fill logic for one route slot, used by both app_add_route_mw and
 * router_add_route_mw - an App and a Router register routes identically,
 * they just land in different fixed Route[MAX_ROUTES] arrays. */
static void fill_route(Route *route, const char *method, const char *path, Handler handler,
                        const Middleware *middlewares, int middleware_count) {
    strncpy(route->method, method, sizeof(route->method) - 1);
    route->method[sizeof(route->method) - 1] = '\0';
    strncpy(route->path, path, sizeof(route->path) - 1);
    route->path[sizeof(route->path) - 1] = '\0';
    route->handler = handler;

    if (middleware_count > MAX_ROUTE_MIDDLEWARES) {
        fprintf(stderr, "route registration: MAX_ROUTE_MIDDLEWARES exceeded, truncating\n");
        middleware_count = MAX_ROUTE_MIDDLEWARES;
    }
    for (int i = 0; i < middleware_count; i++) {
        route->middlewares[i] = middlewares[i];
    }
    route->middleware_count = middleware_count;
}

void app_add_route(App *app, const char *method, const char *path, Handler handler) {
    app_add_route_mw(app, method, path, handler, NULL, 0);
}

void app_add_route_mw(App *app, const char *method, const char *path, Handler handler,
                       const Middleware *middlewares, int middleware_count) {
    if (app->route_count >= MAX_ROUTES) {
        fprintf(stderr, "app_add_route: MAX_ROUTES exceeded\n");
        return;
    }
    fill_route(&app->routes[app->route_count++], method, path, handler, middlewares, middleware_count);
}

void app_get(App *app, const char *path, Handler handler) {
    app_add_route(app, "GET", path, handler);
}

void app_post(App *app, const char *path, Handler handler) {
    app_add_route(app, "POST", path, handler);
}

void app_get_mw(App *app, const char *path, Handler handler,
                 const Middleware *middlewares, int middleware_count) {
    app_add_route_mw(app, "GET", path, handler, middlewares, middleware_count);
}

void app_post_mw(App *app, const char *path, Handler handler,
                  const Middleware *middlewares, int middleware_count) {
    app_add_route_mw(app, "POST", path, handler, middlewares, middleware_count);
}

void app_put(App *app, const char *path, Handler handler) {
    app_add_route(app, "PUT", path, handler);
}

void app_patch(App *app, const char *path, Handler handler) {
    app_add_route(app, "PATCH", path, handler);
}

void app_delete(App *app, const char *path, Handler handler) {
    app_add_route(app, "DELETE", path, handler);
}

void app_put_mw(App *app, const char *path, Handler handler,
                 const Middleware *middlewares, int middleware_count) {
    app_add_route_mw(app, "PUT", path, handler, middlewares, middleware_count);
}

void app_patch_mw(App *app, const char *path, Handler handler,
                   const Middleware *middlewares, int middleware_count) {
    app_add_route_mw(app, "PATCH", path, handler, middlewares, middleware_count);
}

void app_delete_mw(App *app, const char *path, Handler handler,
                    const Middleware *middlewares, int middleware_count) {
    app_add_route_mw(app, "DELETE", path, handler, middlewares, middleware_count);
}

void router_init(Router *router) {
    router->route_count = 0;
    router->middleware_count = 0;
}

void router_add_route(Router *router, const char *method, const char *path, Handler handler) {
    router_add_route_mw(router, method, path, handler, NULL, 0);
}

void router_add_route_mw(Router *router, const char *method, const char *path, Handler handler,
                          const Middleware *middlewares, int middleware_count) {
    if (router->route_count >= MAX_ROUTES) {
        fprintf(stderr, "router_add_route: MAX_ROUTES exceeded\n");
        return;
    }
    fill_route(&router->routes[router->route_count++], method, path, handler, middlewares, middleware_count);
}

void router_get(Router *router, const char *path, Handler handler) {
    router_add_route(router, "GET", path, handler);
}

void router_post(Router *router, const char *path, Handler handler) {
    router_add_route(router, "POST", path, handler);
}

void router_get_mw(Router *router, const char *path, Handler handler,
                    const Middleware *middlewares, int middleware_count) {
    router_add_route_mw(router, "GET", path, handler, middlewares, middleware_count);
}

void router_post_mw(Router *router, const char *path, Handler handler,
                     const Middleware *middlewares, int middleware_count) {
    router_add_route_mw(router, "POST", path, handler, middlewares, middleware_count);
}

void router_put(Router *router, const char *path, Handler handler) {
    router_add_route(router, "PUT", path, handler);
}

void router_patch(Router *router, const char *path, Handler handler) {
    router_add_route(router, "PATCH", path, handler);
}

void router_delete(Router *router, const char *path, Handler handler) {
    router_add_route(router, "DELETE", path, handler);
}

void router_put_mw(Router *router, const char *path, Handler handler,
                    const Middleware *middlewares, int middleware_count) {
    router_add_route_mw(router, "PUT", path, handler, middlewares, middleware_count);
}

void router_patch_mw(Router *router, const char *path, Handler handler,
                      const Middleware *middlewares, int middleware_count) {
    router_add_route_mw(router, "PATCH", path, handler, middlewares, middleware_count);
}

void router_delete_mw(Router *router, const char *path, Handler handler,
                       const Middleware *middlewares, int middleware_count) {
    router_add_route_mw(router, "DELETE", path, handler, middlewares, middleware_count);
}

void router_use(Router *router, Middleware mw) {
    if (router->middleware_count >= MAX_MIDDLEWARES) {
        fprintf(stderr, "router_use: MAX_MIDDLEWARES exceeded\n");
        return;
    }
    router->middlewares[router->middleware_count++] = mw;
}

/* Builds the mounted path for one router route: prefix + route->path, except
 * a router route registered at "/" (the router's own root) mounts at the
 * prefix itself rather than "prefix/" - so router_get(router, "/", h) mounted
 * at "/api" matches "/api", not "/api/". prefix has already been normalized
 * by app_mount (no trailing slash, "" for an unscoped/root mount). */
static void build_mounted_path(char *out, size_t out_size, const char *prefix, const char *route_path) {
    if (strcmp(route_path, "/") == 0) {
        snprintf(out, out_size, "%s", prefix[0] != '\0' ? prefix : "/");
    } else {
        snprintf(out, out_size, "%s%s", prefix, route_path);
    }
}

void app_mount(App *app, const char *prefix, const Router *router) {
    if (prefix == NULL) {
        prefix = "";
    }

    /* Normalize: "/" alone means an unscoped/root mount, same as "" - both
     * app_use_prefix and build_mounted_path treat "" as "no prefix to add/
     * match on". A trailing slash (e.g. "/api/") is stripped so concatenation
     * with a route's own leading-slash path doesn't double up ("/api//users"). */
    char normalized_prefix[128];
    if (prefix[0] == '\0' || strcmp(prefix, "/") == 0) {
        normalized_prefix[0] = '\0';
    } else {
        strncpy(normalized_prefix, prefix, sizeof(normalized_prefix) - 1);
        normalized_prefix[sizeof(normalized_prefix) - 1] = '\0';
        const size_t len = strlen(normalized_prefix);
        if (len > 0 && normalized_prefix[len - 1] == '/') {
            normalized_prefix[len - 1] = '\0';
        }
    }

    for (int i = 0; i < router->middleware_count; i++) {
        app_use_prefix(app, normalized_prefix, router->middlewares[i]);
    }

    for (int i = 0; i < router->route_count; i++) {
        const Route *route = &router->routes[i];
        char mounted_path[256];
        build_mounted_path(mounted_path, sizeof(mounted_path), normalized_prefix, route->path);
        app_add_route_mw(app, route->method, mounted_path, route->handler,
                          route->middlewares, route->middleware_count);
    }
}

int match_path(const char *pattern, const char *path, Request *req) {
    char pattern_copy[256];
    char path_copy[256];
    strncpy(pattern_copy, pattern, sizeof(pattern_copy) - 1);
    pattern_copy[sizeof(pattern_copy) - 1] = '\0';
    strncpy(path_copy, path, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';

    char *pattern_saveptr, *path_saveptr;
    char *pattern_tok = strtok_r(pattern_copy, "/", &pattern_saveptr);
    char *path_tok = strtok_r(path_copy, "/", &path_saveptr);

    req->param_count = 0;

    while (pattern_tok != NULL && path_tok != NULL) {
        if (strcmp(pattern_tok, "*") == 0) {
            char *next_pattern_tok = strtok_r(NULL, "/", &pattern_saveptr);
            if (next_pattern_tok == NULL) {
                /* Trailing wildcard (a pattern ending in "/files/" plus a
                 * trailing "*"): matches this segment and every segment
                 * after it, so the rest of path never needs checking. Not
                 * captured as a param - unlike ":name", "*" has no name to
                 * capture under. */
                return 1;
            }
            /* Mid-path wildcard (e.g. "/users/", "*", "/edit"): matches
             * exactly this one segment, then matching resumes normally
             * against the rest of the pattern. */
            pattern_tok = next_pattern_tok;
            path_tok = strtok_r(NULL, "/", &path_saveptr);
            continue;
        }
        if (pattern_tok[0] == ':') {
            if (req->param_count < MAX_PARAMS) {
                strncpy(req->param_names[req->param_count], pattern_tok + 1,
                        sizeof(req->param_names[0]) - 1);
                strncpy(req->param_values[req->param_count], path_tok,
                        sizeof(req->param_values[0]) - 1);
                req->param_count++;
            }
        } else if (strcmp(pattern_tok, path_tok) != 0) {
            return 0;
        }
        pattern_tok = strtok_r(NULL, "/", &pattern_saveptr);
        path_tok = strtok_r(NULL, "/", &path_saveptr);
    }

    /* Both must be fully consumed, otherwise one path is longer than the other. */
    return pattern_tok == NULL && path_tok == NULL;
}

const Route *match_route(const App *app, Request *req) {
    for (int i = 0; i < app->route_count; i++) {
        const Route *route = &app->routes[i];
        if (strcmp(route->method, req->method) != 0) {
            continue;
        }
        if (match_path(route->path, req->path, req)) {
            return route;
        }
    }
    return NULL;
}

int match_route_allowed_methods(const App *app, const Request *req, char *allowed, size_t allowed_size) {
    Request scratch = *req;
    char seen[MAX_ROUTES][8];
    int seen_count = 0;
    allowed[0] = '\0';

    for (int i = 0; i < app->route_count; i++) {
        const Route *route = &app->routes[i];
        if (!match_path(route->path, req->path, &scratch)) {
            continue;
        }

        int already_seen = 0;
        for (int j = 0; j < seen_count; j++) {
            if (strcmp(seen[j], route->method) == 0) {
                already_seen = 1;
                break;
            }
        }
        if (already_seen) {
            continue;
        }
        strncpy(seen[seen_count], route->method, sizeof(seen[0]) - 1);
        seen[seen_count][sizeof(seen[0]) - 1] = '\0';
        seen_count++;

        if (allowed[0] != '\0') {
            strncat(allowed, ", ", allowed_size - strlen(allowed) - 1);
        }
        strncat(allowed, route->method, allowed_size - strlen(allowed) - 1);
    }

    return seen_count;
}

const char *req_get_param(const Request *req, const char *name) {
    for (int i = 0; i < req->param_count; i++) {
        if (strcmp(req->param_names[i], name) == 0) {
            return req->param_values[i];
        }
    }
    return NULL;
}
