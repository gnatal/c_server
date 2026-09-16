#include <stdio.h>
#include <stdlib.h>
#include "handlers.h"
#include "router.h"
#include "response.h"
#include "json/json.h"

void handler_home(const Request *req, Response *res) {
    (void)req;
    res_send(res, "Welcome to the home page!\n");
}

void handler_get_user(const Request *req, Response *res) {
    const char *id = req_get_param(req, "id");
    char body[128];
    snprintf(body, sizeof(body), "User id: %s\n", id != NULL ? id : "unknown");
    res_send(res, body);
}

void handler_echo(const Request *req, Response *res) {
    res_send(res, req->body);
}

void handler_api_status(const Request *req, Response *res) {
    (void)req;
    res_json(res, "{\"status\":\"ok\"}");
}

void handler_echo_json(const Request *req, Response *res) {
    char err[128];
    JsonValue *body = json_parse(req->body, err, sizeof(err));
    if (body == NULL) {
        char error_json[256];
        snprintf(error_json, sizeof(error_json), "{\"error\":\"%s\"}", err);
        res_status(res, 400);
        res_json(res, error_json);
        return;
    }

    char *out = json_stringify(body);
    res_json(res, out != NULL ? out : "null");

    free(out);
    json_free(body);
}
