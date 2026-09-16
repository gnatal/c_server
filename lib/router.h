#ifndef ROUTER_H
#define ROUTER_H

#include "app_types.h"

/* Resets an App to have zero registered routes and no open connections. */
void app_init(App *app);

/* Registers a handler for a given HTTP method + path pattern. */
void app_add_route(App *app, const char *method, const char *path, Handler handler);

/* Express-style convenience wrapper: app_get(app, "/users", handler). */
void app_get(App *app, const char *path, Handler handler);

/* Express-style convenience wrapper: app_post(app, "/users", handler). */
void app_post(App *app, const char *path, Handler handler);

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
