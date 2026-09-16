#include <stdio.h>
#include "middleware.h"
#include "response.h"

void app_use(App *app, Middleware mw) {
    if (app->middleware_count >= MAX_MIDDLEWARES) {
        fprintf(stderr, "app_use: MAX_MIDDLEWARES exceeded\n");
        return;
    }
    app->middlewares[app->middleware_count++] = mw;
}

void app_use_error(App *app, ErrorHandler handler) {
    app->error_handler = handler;
}

void chain_next(MiddlewareChain *chain) {
    if (chain->index < chain->count) {
        const Middleware mw = chain->middlewares[chain->index++];
        mw(chain->req, chain->res, chain);
        return;
    }

    const int route_index = chain->index - chain->count;
    if (route_index < chain->route_middleware_count) {
        const Middleware mw = chain->route_middlewares[route_index];
        chain->index++;
        mw(chain->req, chain->res, chain);
        return;
    }

    if (chain->final_handler != NULL) {
        chain->final_handler(chain->req, chain->res);
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
    MiddlewareChain chain = {
        .middlewares = app->middlewares,
        .count = app->middleware_count,
        .route_middlewares = route != NULL ? route->middlewares : NULL,
        .route_middleware_count = route != NULL ? route->middleware_count : 0,
        .index = 0,
        .final_handler = route != NULL ? route->handler : NULL,
        .error_handler = app->error_handler,
        .req = req,
        .res = res,
    };
    chain_next(&chain);
}
