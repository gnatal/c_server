#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "handlers.h"
#include "router.h"
#include "response.h"
#include "http_parser.h"
#include "multipart.h"
#include "urlencoded.h"
#include "json/json.h"

void handler_home(const Request *req, Response *res) {
    (void)req;
    res_send(res, "Welcome to the home page!\n");
}

/*
 * Redirect demo (lib/response.h: res_redirect): GET /old-home permanently
 * redirects to "/" via an explicit 301 - see lib/response.c: res_redirect
 * for the implicit-302-default behavior (res_redirect(res, 0, ...)) this
 * demo doesn't need since it wants a permanent redirect specifically.
 */
void handler_old_home(const Request *req, Response *res) {
    (void)req;
    res_redirect(res, 301, "/");
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

/*
 * Cookie demo (lib/response.h: res_set_cookie / lib/http_parser.h:
 * req_get_cookie): GET /login sets a session cookie, GET /whoami reads it
 * back, GET /logout expires it. HttpOnly + a 1-hour Max-Age here are just
 * demo choices, not requirements of res_set_cookie itself.
 */
void handler_login(const Request *req, Response *res) {
    (void)req;
    const CookieOptions options = { .max_age = 3600, .path = "/", .http_only = 1,
                                     .same_site = COOKIE_SAMESITE_LAX };
    res_set_cookie(res, "session", "demo-session-token", &options);
    res_send(res, "Logged in - session cookie set\n");
}

void handler_whoami(const Request *req, Response *res) {
    const char *session = req_get_cookie(req, "session");
    char body[128];
    snprintf(body, sizeof(body), "session: %s\n", session != NULL ? session : "(none - not logged in)");
    res_send(res, body);
}

void handler_logout(const Request *req, Response *res) {
    (void)req;
    res_clear_cookie(res, "session", "/");
    res_send(res, "Logged out - session cookie cleared\n");
}

/* True when Content-Type declares a JSON body - an optional ";charset=..."
 * (or any other) suffix is ignored, only the media-type prefix matters.
 * Gates JSON validation below so a non-JSON body (plain text, form data)
 * isn't rejected as "broken JSON" just because it fails to parse as one. */
static int has_json_content_type(const Request *req) {
    const char *content_type = req_get_header(req, "Content-Type");
    if (content_type == NULL) {
        return 0;
    }
    return strncasecmp(content_type, "application/json", strlen("application/json")) == 0;
}

void handler_update_user(const Request *req, Response *res) {
    if (has_json_content_type(req)) {
        char err[128];
        JsonValue *body = json_parse(req->body, err, sizeof(err));
        if (body == NULL) {
            char error_json[256];
            snprintf(error_json, sizeof(error_json), "{\"error\":\"%s\"}", err);
            res_status(res, 400);
            res_json(res, error_json);
            return;
        }
        json_free(body);
    }

    const char *id = req_get_param(req, "id");
    char body[128];
    snprintf(body, sizeof(body), "User %s replaced (PUT)\n", id != NULL ? id : "unknown");
    res_send(res, body);
}

void handler_patch_user(const Request *req, Response *res) {
    if (has_json_content_type(req)) {
        char err[128];
        JsonValue *body = json_parse(req->body, err, sizeof(err));
        if (body == NULL) {
            char error_json[256];
            snprintf(error_json, sizeof(error_json), "{\"error\":\"%s\"}", err);
            res_status(res, 400);
            res_json(res, error_json);
            return;
        }
        json_free(body);
    }

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
     * as-is from req->query_values (already URL-decoded by parse_query_string,
     * lib/http_parser.c) with no further type coercion - everything comes
     * back as a JSON string. */
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

/*
 * Test-only: dumps a file part's raw bytes to a fixed path in the server's
 * working directory, overwriting whatever was there before - there's no
 * MAX_UPLOAD_PATH or per-request naming, this is deliberately not a real
 * upload-storage feature (no client-controlled filename ever touches the
 * filesystem, which also sidesteps any path-traversal concern from
 * part->filename). Returns 1 on success, 0 if the file couldn't be opened
 * or the write was incomplete - either way the caller still returns the
 * parsed-part JSON, just without a "saved_as" entry.
 */
static int save_part_to_temp_file(const MultipartPart *part, const char *path) {
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return 0;
    }
    size_t written = fwrite(part->data, 1, part->data_len, f);
    fclose(f);
    return written == part->data_len;
}

/*
 * Demonstrates multipart/form-data parsing (lib/multipart.h): echoes every
 * parsed part back as a JSON object keyed by its form field name - a plain
 * field becomes a JSON string of its value, a file part (one with a
 * filename) becomes {"filename", "content_type", "size"} instead of its
 * raw bytes, since a file's payload can be arbitrary binary data that isn't
 * valid to embed as a JSON string as-is. The first file part encountered is
 * also written to disk as "temp.txt" (see save_part_to_temp_file) purely to
 * exercise/verify upload handling by hand - not a real storage feature.
 */
void handler_upload(const Request *req, Response *res) {
    static const char *const TEMP_UPLOAD_PATH = "temp.txt";

    char boundary[MAX_BOUNDARY_LEN + 1];
    if (!multipart_parse_boundary(req_get_header(req, "Content-Type"), boundary, sizeof(boundary))) {
        res_status(res, 400);
        res_json(res, "{\"error\":\"expected multipart/form-data with a boundary\"}");
        return;
    }

    MultipartForm form;
    parse_multipart_body(req->body, (size_t)req->content_length, boundary, &form);

    JsonValue *body = json_new_object();
    for (int i = 0; i < form.part_count && body != NULL; i++) {
        const MultipartPart *part = &form.parts[i];
        JsonValue *entry;
        if (part->filename[0] != '\0') {
            entry = json_new_object();
            json_object_set(entry, "filename", json_new_string(part->filename));
            json_object_set(entry, "content_type", json_new_string(part->content_type));
            char size_str[32];
            snprintf(size_str, sizeof(size_str), "%zu", part->data_len);
            json_object_set(entry, "size", json_new_string(size_str));
            if (save_part_to_temp_file(part, TEMP_UPLOAD_PATH)) {
                json_object_set(entry, "saved_as", json_new_string(TEMP_UPLOAD_PATH));
            }
        } else {
            /* part->data is not NUL-terminated (lib/multipart.h) - copy it
             * into a bounded, NUL-terminated buffer before handing it to
             * json_new_string, which expects a C string. Truncated past
             * this demo buffer's size rather than growing unbounded. */
            char value[256];
            size_t len = part->data_len < sizeof(value) - 1 ? part->data_len : sizeof(value) - 1;
            memcpy(value, part->data, len);
            value[len] = '\0';
            entry = json_new_string(value);
        }
        json_object_set(body, part->name, entry);
    }

    char *out = json_stringify(body);
    res_json(res, out != NULL ? out : "null");

    free(out);
    json_free(body);
}

/* True when Content-Type declares an application/x-www-form-urlencoded
 * body, same prefix-match convention as has_json_content_type above (a
 * trailing ";charset=..." doesn't break the match). Gates form parsing
 * below so a non-form body isn't misinterpreted as one. */
static int has_urlencoded_content_type(const Request *req) {
    const char *content_type = req_get_header(req, "Content-Type");
    if (content_type == NULL) {
        return 0;
    }
    return strncasecmp(content_type, "application/x-www-form-urlencoded",
                        strlen("application/x-www-form-urlencoded")) == 0;
}

/*
 * Demonstrates application/x-www-form-urlencoded parsing
 * (lib/urlencoded.h): echoes every parsed field back as a JSON object, e.g.
 * a body of "name=natal&age=32" -> {"name":"natal","age":"32"} - the same
 * shape handler_search (above) builds from req->query_names/query_values,
 * since the two share the same key=value&key2=value2 grammar and decoding
 * rules, just carried in the body instead of the URL.
 */
void handler_form(const Request *req, Response *res) {
    if (!has_urlencoded_content_type(req)) {
        res_status(res, 400);
        res_json(res, "{\"error\":\"expected application/x-www-form-urlencoded\"}");
        return;
    }

    UrlEncodedForm form;
    parse_urlencoded_body(req->body, (size_t)req->content_length, &form);

    JsonValue *body = json_new_object();
    for (int i = 0; i < form.field_count && body != NULL; i++) {
        json_object_set(body, form.field_names[i], json_new_string(form.field_values[i]));
    }

    char *out = json_stringify(body);
    res_json(res, out != NULL ? out : "null");

    free(out);
    json_free(body);
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

void handler_stream(const Request *req, Response *res) {
    (void)req;
    res_set_header(res, "Content-Type", "text/plain");
    res_set_trailer(res, "Server-Timing", "demo;dur=12.5");
    res_write(res, "chunk 1: hello\n", 15);
    res_write(res, "chunk 2: streaming world\n", 25);
    res_write(res, "chunk 3: done\n", 14);
    res_end(res);
}

void handler_download(const Request *req, Response *res) {
    (void)req;
    if (res_send_file(res, "text/markdown", "README.md") != 0) {
        res_status(res, 404);
        res_send(res, "File Not Found");
    }
}
