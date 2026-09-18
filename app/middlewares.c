#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "middlewares.h"
#include "router.h"
#include "response.h"
#include "middleware.h"
#include "http_parser.h"
#include "json/json.h"

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
    JsonWriter w;
    jw_init(&w);
    jw_object_begin(&w);
    jw_key(&w, "error");
    jw_string(&w, message); /* escaped, unlike snprintf("%s") */
    jw_object_end(&w);
    res_status(res, status);
    res_json(res, jw_ok(&w) ? jw_data(&w) : "{\"error\":\"internal error\"}");
    jw_free(&w);
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