#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "middlewares.h"
#include "router.h"
#include "response.h"
#include "middleware.h"
#include "http_parser.h"
#include "vendor/yyjson/yyjson.h"

static const char *default_api_key = "my-secret-api-key";
static const char *configured_api_key = NULL;

void mw_authenticate_set_key(const char *key) {
    configured_api_key = key;
}

const char *mw_authenticate_get_key(void) {
    if (configured_api_key != NULL) {
        return configured_api_key;
    }
    const char *env_key = getenv("API_KEY");
    if (env_key != NULL && env_key[0] != '\0') {
        return env_key;
    }
    return default_api_key;
}

void mw_logger(const Request *req, Response *res, MiddlewareChain *chain) {
    chain_next(chain);
    printf("%s %s -> %d\n", req->method, req->path, res->status);
}

void mw_body_size_guard(const Request *req, Response *res, MiddlewareChain *chain) {
    (void)res;
    if (req->content_length > MAX_APP_BODY_SIZE) {
        chain_error(chain, 400, "Request body exceeds this application's size limit");
        return;
    }
    chain_next(chain);
}

void error_handler_json(int status, const char *message, const Request *req, Response *res) {
    (void)req;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, obj);
    yyjson_mut_obj_add_str(doc, obj, "error", message);

    res_status(res, status);
    size_t len;
    char *json = yyjson_mut_write(doc, 0, &len);
    if (json) {
        res_json(res, json);
        free(json);
    } else {
        res_json(res, "{\"error\":\"internal error\"}");
    }
    yyjson_mut_doc_free(doc);
}

/* Constant-time comparison so a wrong API key can't be distinguished from a
 * right one by how quickly the check fails (timing side-channel on the
 * secret) - unlike strcmp, this always inspects every byte of the shorter
 * length-bounded run before returning. */
static int keys_match(const char *a, size_t a_len, const char *b, size_t b_len) {
    if (a_len != b_len) {
        return 0;
    }
    unsigned char diff = 0;
    for (size_t i = 0; i < a_len; i++) {
        diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
    }
    return diff == 0;
}

void mw_authenticate(const Request *req, Response *res, MiddlewareChain *chain) {
    (void)res;
    const char *auth_header = req_get_header(req, "Authorization");
    if (auth_header == NULL) {
        chain_error(chain, 401, "Unauthorized: Missing Authorization header");
        return;
    }

    static const char prefix[] = "Bearer ";
    static const size_t prefix_len = sizeof(prefix) - 1;
    if (strncmp(auth_header, prefix, prefix_len) != 0) {
        chain_error(chain, 401, "Unauthorized: Authorization header must be a Bearer token");
        return;
    }
    const char *token = auth_header + prefix_len;

    const char *expected_key = mw_authenticate_get_key();
    if (!keys_match(token, strlen(token), expected_key, strlen(expected_key))) {
        chain_error(chain, 401, "Unauthorized: Invalid API key");
        return;
    }

    chain_next(chain);
}