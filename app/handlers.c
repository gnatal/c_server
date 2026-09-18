#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "handlers.h"
#include "router.h"
#include "response.h"
#include "http_parser.h"
#include "json/json.h"
#include "db.h"

static void send_error(Response *res, int status, const char *message) {
    char body[256];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", message);
    res_status(res, status);
    res_json(res, body);
}

/* True when Content-Type declares a JSON body - an optional ";charset=..."
 * (or any other) suffix is ignored, only the media-type prefix matters. */
static int has_json_content_type(const Request *req) {
    const char *content_type = req_get_header(req, "Content-Type");
    if (content_type == NULL) {
        return 0;
    }
    return strncasecmp(content_type, "application/json", strlen("application/json")) == 0;
}

/* Parses the ":id" path param as a non-negative integer. Returns 0 and
 * fills *out on success, -1 on a missing/malformed/negative id. */
static int parse_id_param(const Request *req, long long *out) {
    const char *id_str = req_get_param(req, "id");
    if (id_str == NULL || id_str[0] == '\0') {
        return -1;
    }
    char *end = NULL;
    errno = 0;
    long long value = strtoll(id_str, &end, 10);
    if (errno != 0 || end == id_str || *end != '\0' || value < 0) {
        return -1;
    }
    *out = value;
    return 0;
}

static JsonValue *todo_to_json(const Todo *todo) {
    JsonValue *obj = json_new_object();
    json_object_set(obj, "id", json_new_number((double)todo->id));
    json_object_set(obj, "title", json_new_string(todo->title));
    json_object_set(obj, "done", json_new_bool(todo->done));
    json_object_set(obj, "created_at", json_new_string(todo->created_at));
    json_object_set(obj, "updated_at", json_new_string(todo->updated_at));
    return obj;
}

static void send_todo_json(Response *res, int status, const Todo *todo) {
    JsonValue *obj = todo_to_json(todo);
    char *out = json_stringify(obj);
    res_status(res, status);
    res_json(res, out != NULL ? out : "null");
    free(out);
    json_free(obj);
}

void handler_home(const Request *req, Response *res) {
    (void)req;
    if (res_send_file(res, "text/html", "app/public/index.html") != 0) {
        res_status(res, 404);
        res_send(res, "Not Found");
    }
}

void handler_list_todos(const Request *req, Response *res) {
    /* ?done=true|false narrows the list; any other value (or absent) means
     * unfiltered - same lenient-ignore convention as the rest of this
     * project's query-string handling. */
    int filter_value = 0;
    const int *filter = NULL;
    const char *done_q = req_get_query(req, "done");
    if (done_q != NULL) {
        if (strcmp(done_q, "true") == 0) {
            filter_value = 1;
            filter = &filter_value;
        } else if (strcmp(done_q, "false") == 0) {
            filter_value = 0;
            filter = &filter_value;
        }
    }

    TodoList list;
    if (db_list_todos(&list, filter) != 0) {
        send_error(res, 500, "failed to list todos");
        return;
    }

    JsonValue *arr = json_new_array();
    for (int i = 0; i < list.count && arr != NULL; i++) {
        json_array_append(arr, todo_to_json(&list.items[i]));
    }

    char *out = json_stringify(arr);
    res_json(res, out != NULL ? out : "null");

    free(out);
    json_free(arr);
}

void handler_get_todo(const Request *req, Response *res) {
    long long id;
    if (parse_id_param(req, &id) != 0) {
        send_error(res, 400, "invalid id");
        return;
    }

    Todo todo;
    int found = db_get_todo(id, &todo);
    if (found < 0) {
        send_error(res, 500, "failed to load todo");
        return;
    }
    if (found == 0) {
        send_error(res, 404, "todo not found");
        return;
    }
    send_todo_json(res, 200, &todo);
}

void handler_create_todo(const Request *req, Response *res) {
    if (!has_json_content_type(req)) {
        send_error(res, 400, "expected application/json");
        return;
    }

    char err[128];
    JsonValue *body = json_parse(req->body, err, sizeof(err));
    if (body == NULL) {
        send_error(res, 400, err);
        return;
    }

    const char *title = json_as_string(json_object_get(body, "title"), NULL);
    if (!todo_title_is_valid(title)) {
        json_free(body);
        send_error(res, 400, "title is required and must be 1-255 characters");
        return;
    }

    Todo todo;
    int rc = db_create_todo(title, &todo);
    json_free(body);
    if (rc != 0) {
        send_error(res, 500, "failed to create todo");
        return;
    }
    send_todo_json(res, 201, &todo);
}

void handler_replace_todo(const Request *req, Response *res) {
    long long id;
    if (parse_id_param(req, &id) != 0) {
        send_error(res, 400, "invalid id");
        return;
    }
    if (!has_json_content_type(req)) {
        send_error(res, 400, "expected application/json");
        return;
    }

    char err[128];
    JsonValue *body = json_parse(req->body, err, sizeof(err));
    if (body == NULL) {
        send_error(res, 400, err);
        return;
    }

    const char *title = json_as_string(json_object_get(body, "title"), NULL);
    const JsonValue *done_value = json_object_get(body, "done");
    if (!todo_title_is_valid(title) || done_value == NULL) {
        json_free(body);
        send_error(res, 400, "title and done are both required for a full replace");
        return;
    }
    int done = json_as_bool(done_value, 0);

    Todo todo;
    int result = db_replace_todo(id, title, done, &todo);
    json_free(body);
    if (result < 0) {
        send_error(res, 500, "failed to update todo");
        return;
    }
    if (result == 0) {
        send_error(res, 404, "todo not found");
        return;
    }
    send_todo_json(res, 200, &todo);
}

void handler_patch_todo(const Request *req, Response *res) {
    long long id;
    if (parse_id_param(req, &id) != 0) {
        send_error(res, 400, "invalid id");
        return;
    }
    if (!has_json_content_type(req)) {
        send_error(res, 400, "expected application/json");
        return;
    }

    char err[128];
    JsonValue *body = json_parse(req->body, err, sizeof(err));
    if (body == NULL) {
        send_error(res, 400, err);
        return;
    }

    const JsonValue *title_value = json_object_get(body, "title");
    const char *title = json_as_string(title_value, NULL);
    if (title_value != NULL && !todo_title_is_valid(title)) {
        json_free(body);
        send_error(res, 400, "title must be 1-255 characters");
        return;
    }

    const JsonValue *done_value = json_object_get(body, "done");
    int done_storage = 0;
    const int *done = NULL;
    if (done_value != NULL) {
        done_storage = json_as_bool(done_value, 0);
        done = &done_storage;
    }

    Todo todo;
    int result = db_patch_todo(id, title, done, &todo);
    json_free(body);
    if (result < 0) {
        send_error(res, 500, "failed to update todo");
        return;
    }
    if (result == 0) {
        send_error(res, 404, "todo not found");
        return;
    }
    send_todo_json(res, 200, &todo);
}

void handler_delete_todo(const Request *req, Response *res) {
    long long id;
    if (parse_id_param(req, &id) != 0) {
        send_error(res, 400, "invalid id");
        return;
    }

    int result = db_delete_todo(id);
    if (result < 0) {
        send_error(res, 500, "failed to delete todo");
        return;
    }
    if (result == 0) {
        send_error(res, 404, "todo not found");
        return;
    }
    /* 204 No Content: delete succeeded, nothing to return. */
    res_status(res, 204);
    res_send(res, "");
}
