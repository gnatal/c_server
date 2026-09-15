#include <stdio.h>
#include <string.h>
#include "middlewares.h"
#include "router.h"
#include "response.h"
#include "middleware.h"

/* In a real application this should come from config/environment, not be
 * hard-coded in source. */
static const char *api_key = "my-secret-api-key";

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
    char body[512];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", message);
    res_status(res, status);
    res_json(res, body);
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
    const char *auth_header = strstr(req->headers, "Authorization:");
    if (auth_header == NULL) {
        chain_error(chain, 401, "Unauthorized: Missing Authorization header");
        return;
    }

    const char *value = auth_header + strlen("Authorization:");
    while (*value == ' ') {
        value++;
    }

    static const char prefix[] = "Bearer ";
    static const size_t prefix_len = sizeof(prefix) - 1;
    if (strncmp(value, prefix, prefix_len) != 0) {
        chain_error(chain, 401, "Unauthorized: Authorization header must be a Bearer token");
        return;
    }
    value += prefix_len;

    const char *token_end = strstr(value, "\r\n");
    const size_t token_len = token_end != NULL ? (size_t)(token_end - value) : strlen(value);

    if (!keys_match(value, token_len, api_key, strlen(api_key))) {
        chain_error(chain, 401, "Unauthorized: Invalid API key");
        return;
    }

    chain_next(chain);
}