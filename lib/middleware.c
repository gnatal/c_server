#include <stdio.h>
#include <string.h>
#include "middleware.h"
#include "response.h"
#include "router.h"
#include "static.h"

/* prefix "" or "/" is treated as unscoped (matches every path), same as an
 * app_use() call before prefix-scoping existed. Otherwise path must start
 * with prefix and either end there or be followed by '/', so "/api" matches
 * "/api"/"/api/foo" but not "/apiary". */
static int middleware_prefix_matches(const char *prefix, const char *path) {
    if (prefix[0] == '\0' || (prefix[0] == '/' && prefix[1] == '\0')) {
        return 1;
    }
    const size_t prefix_len = strlen(prefix);
    if (strncmp(path, prefix, prefix_len) != 0) {
        return 0;
    }
    const char next = path[prefix_len];
    return next == '\0' || next == '/';
}

/* Checks whether method (e.g. "HEAD") appears as its own comma-separated
 * token in list (e.g. "GET, POST"), not just as a substring - used below to
 * decide whether to append an implicit method to an auto-OPTIONS Allow
 * header without double-adding one a route already registered explicitly. */
static int method_list_contains(const char *list, const char *method) {
    const size_t method_len = strlen(method);
    const char *p = list;
    while ((p = strstr(p, method)) != NULL) {
        const int start_ok = (p == list) || (p[-1] == ' ');
        const int end_ok = (p[method_len] == '\0') || (p[method_len] == ',');
        if (start_ok && end_ok) {
            return 1;
        }
        p += method_len;
    }
    return 0;
}

void app_use(App *app, Middleware mw) {
    app_use_prefix(app, "", mw);
}

void app_use_prefix(App *app, const char *prefix, Middleware mw) {
    if (app->middleware_count >= MAX_MIDDLEWARES) {
        fprintf(stderr, "app_use: MAX_MIDDLEWARES exceeded\n");
        return;
    }
    if (prefix == NULL) {
        prefix = "";
    }
    MiddlewareEntry *entry = &app->middlewares[app->middleware_count++];
    entry->fn = mw;
    strncpy(entry->prefix, prefix, sizeof(entry->prefix) - 1);
    entry->prefix[sizeof(entry->prefix) - 1] = '\0';
}

void app_use_error(App *app, ErrorHandler handler) {
    app->error_handler = handler;
}

void chain_next(MiddlewareChain *chain) {
    while (chain->index < chain->count) {
        const MiddlewareEntry *entry = &chain->middlewares[chain->index++];
        if (!middleware_prefix_matches(entry->prefix, chain->req->path)) {
            continue;
        }
        entry->fn(chain->req, chain->res, chain);
        return;
    }

    const int route_index = chain->index - chain->count;
    if (route_index < chain->route_middleware_count) {
        const Middleware mw = chain->route_middlewares[route_index];
        chain->index++;
        mw(chain->req, chain->res, chain);
        return;
    }

    if (chain->route != NULL && chain->route->static_root != NULL) {
        /* A static-file mount (app_serve_static, router.h) has no Handler -
         * route->handler is NULL, a plain Handler(req, res) has no way to
         * receive the mount's root directory anyway - so this is checked
         * ahead of final_handler below rather than through it. */
        static_serve_file(chain->route, chain->req, chain->res);
    } else if (chain->final_handler != NULL) {
        chain->final_handler(chain->req, chain->res);
    } else if (chain->method_not_allowed && strcmp(chain->req->method, "OPTIONS") == 0) {
        /* Auto-OPTIONS: mirrors Express, which answers OPTIONS for any path
         * that has at least one route registered (under any method) without
         * needing an explicit app_options() handler for it - an explicit one
         * still wins, since match_route (router.c) tries an exact method
         * match first and only leaves final_handler NULL when that fails.
         * chain->allowed_methods was already computed by dispatch() via
         * match_route_allowed_methods for the 405 branch below; reused here
         * instead. HEAD is folded in whenever GET is registered (it works
         * via match_route's auto-HEAD-from-GET fallback even without its own
         * route), and OPTIONS itself is always implicitly supported once
         * we've reached this branch. */
        char allow[96];
        strncpy(allow, chain->allowed_methods, sizeof(allow) - 1);
        allow[sizeof(allow) - 1] = '\0';
        if (method_list_contains(allow, "GET") && !method_list_contains(allow, "HEAD")) {
            strncat(allow, ", HEAD", sizeof(allow) - strlen(allow) - 1);
        }
        if (!method_list_contains(allow, "OPTIONS")) {
            strncat(allow, ", OPTIONS", sizeof(allow) - strlen(allow) - 1);
        }
        res_set_header(chain->res, "Allow", allow);
        res_status(chain->res, 200);
        res_send(chain->res, "");
    } else if (chain->method_not_allowed) {
        res_set_header(chain->res, "Allow", chain->allowed_methods);
        res_status(chain->res, 405);
        res_send(chain->res, "Method Not Allowed");
    } else {
        res_status(chain->res, 404);
        res_send(chain->res, "Not Found");
    }
}

void chain_error(MiddlewareChain *chain, int status, const char *message) {
    if (chain->error_handler != NULL) {
        chain->error_handler(status, message, chain->req, chain->res);
    } else {
        res_status(chain->res, status);
        res_send(chain->res, message);
    }
}

void dispatch(App *app, const Route *route, const Request *req, Response *res) {
    char allowed_methods[64] = "";
    int method_not_allowed = 0;
    if (route == NULL) {
        method_not_allowed = match_route_allowed_methods(app, req, allowed_methods, sizeof(allowed_methods)) > 0;
    }

    MiddlewareChain chain = {
        .middlewares = app->middlewares,
        .count = app->middleware_count,
        .route_middlewares = route != NULL ? route->middlewares : NULL,
        .route_middleware_count = route != NULL ? route->middleware_count : 0,
        .index = 0,
        .final_handler = route != NULL ? route->handler : NULL,
        .route = route,
        .error_handler = app->error_handler,
        .req = req,
        .res = res,
        .method_not_allowed = method_not_allowed,
        .allowed_methods = allowed_methods,
    };
    chain_next(&chain);
}
