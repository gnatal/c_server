#include <stdio.h>
#include <string.h>
#include "router.h"

void app_init(App *app) {
    app->route_count = 0;
    app->middleware_count = 0;
    app->error_handler = NULL;
    app->server_fd = -1;
    app->kq = -1;
    memset(app->connections, 0, sizeof(app->connections));
}

void app_add_route(App *app, const char *method, const char *path, Handler handler) {
    if (app->route_count >= MAX_ROUTES) {
        fprintf(stderr, "app_add_route: MAX_ROUTES exceeded\n");
        return;
    }
    Route *route = &app->routes[app->route_count++];
    strncpy(route->method, method, sizeof(route->method) - 1);
    route->method[sizeof(route->method) - 1] = '\0';
    strncpy(route->path, path, sizeof(route->path) - 1);
    route->path[sizeof(route->path) - 1] = '\0';
    route->handler = handler;
}

void app_get(App *app, const char *path, Handler handler) {
    app_add_route(app, "GET", path, handler);
}

void app_post(App *app, const char *path, Handler handler) {
    app_add_route(app, "POST", path, handler);
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

const char *req_get_param(const Request *req, const char *name) {
    for (int i = 0; i < req->param_count; i++) {
        if (strcmp(req->param_names[i], name) == 0) {
            return req->param_values[i];
        }
    }
    return NULL;
}
