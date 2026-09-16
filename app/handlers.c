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

void handler_update_user(const Request *req, Response *res) {
    const char *id = req_get_param(req, "id");
    char body[128];
    snprintf(body, sizeof(body), "User %s replaced (PUT)\n", id != NULL ? id : "unknown");
    res_send(res, body);
}

void handler_patch_user(const Request *req, Response *res) {
    const char *id = req_get_param(req, "id");
    char body[128];
    snprintf(body, sizeof(body), "User %s patched (PATCH)\n", id != NULL ? id : "unknown");
    res_send(res, body);
}

void handler_delete_user(const Request *req, Response *res) {
    (void)req;
    /* 204 No Content: DELETE succeeded, nothing to return - status_text()
     * (lib/http_parser.c) already covers 204. */
    res_status(res, 204);
    res_send(res, "");
}

void handler_search(const Request *req, Response *res) {
    /* Echoes every query-string param back as a JSON object, e.g.
     * ?name=natal&age=32 -> {"name":"natal","age":"32"}. Values are taken
     * as-is from req->query_values - no URL-decoding (pending.txt) and no
     * type coercion, everything comes back as a JSON string. */
    JsonValue *body = json_new_object();
    for (int i = 0; i < req->query_count && body != NULL; i++) {
        json_object_set(body, req->query_names[i], json_new_string(req->query_values[i]));
    }

    char *out = json_stringify(body);
    res_json(res, out != NULL ? out : "null");

    free(out);
    json_free(body);
}

void handler_files(const Request *req, Response *res) {
    char body[300];
    snprintf(body, sizeof(body), "Serving: %s\n", req->path);
    res_send(res, body);
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
