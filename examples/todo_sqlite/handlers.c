#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "handlers.h"
#include "router.h"
#include "response.h"
#include "http_parser.h"
#include "vendor/yyjson/yyjson.h"
#include "db.h"

/* Sends {"error": message} with `status`. JsonWriter escapes the message. */
static void send_error(Response *res, int status, const char *message) {
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

/* Writes one todo as a JSON object into `w` (no allocation beyond the writer's own buffer). */
static yyjson_mut_val *write_todo(yyjson_mut_doc *doc, const Todo *todo) {
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc, obj, "id", todo->id);
    yyjson_mut_obj_add_str(doc, obj, "title", todo->title);
    yyjson_mut_obj_add_bool(doc, obj, "done", todo->done);
    yyjson_mut_obj_add_str(doc, obj, "created_at", todo->created_at);
    yyjson_mut_obj_add_str(doc, obj, "updated_at", todo->updated_at);
    return obj;
}

static void send_todo_json(Response *res, int status, const Todo *todo) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_doc_set_root(doc, write_todo(doc, todo));
    res_status(res, status);
    size_t len;
    char *json = yyjson_mut_write(doc, 0, &len);
    if (json) {
        res_json(res, json);
        free(json);
    } else {
        send_error(res, 500, "failed to encode todo");
    }
    yyjson_mut_doc_free(doc);
}

void handler_home(const Request *req, Response *res) {
    (void)req;
    if (res_send_file(res, "text/html", "public/index.html") != 0) {
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

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    yyjson_mut_doc_set_root(doc, arr);
    for (int i = 0; i < list.count; i++) {
        yyjson_mut_arr_append(arr, write_todo(doc, &list.items[i]));
    }
    size_t len;
    char *json = yyjson_mut_write(doc, 0, &len);
    if (json) {
        res_json(res, json);
        free(json);
    } else {
        send_error(res, 500, "failed to encode todos");
    }
    yyjson_mut_doc_free(doc);
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

    yyjson_doc *doc = yyjson_read(req->body, strlen(req->body), 0);
    if (doc == NULL) {
        send_error(res, 400, "invalid json");
        return;
    }
    yyjson_val *body = yyjson_doc_get_root(doc);

    const char *title = yyjson_get_str(yyjson_obj_get(body, "title"));
    if (!todo_title_is_valid(title)) {
        yyjson_doc_free(doc);
        send_error(res, 400, "title is required and must be 1-255 characters");
        return;
    }

    Todo todo;
    int rc = db_create_todo(title, &todo);
    yyjson_doc_free(doc);
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

    yyjson_doc *doc = yyjson_read(req->body, strlen(req->body), 0);
    if (doc == NULL) {
        send_error(res, 400, "invalid json");
        return;
    }
    yyjson_val *body = yyjson_doc_get_root(doc);

    const char *title = yyjson_get_str(yyjson_obj_get(body, "title"));
    yyjson_val *done_value = yyjson_obj_get(body, "done");
    if (!todo_title_is_valid(title) || done_value == NULL) {
        yyjson_doc_free(doc);
        send_error(res, 400, "title and done are both required for a full replace");
        return;
    }
    int done = yyjson_get_bool(done_value);

    Todo todo;
    int result = db_replace_todo(id, title, done, &todo);
    yyjson_doc_free(doc);
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

    yyjson_doc *doc = yyjson_read(req->body, strlen(req->body), 0);
    if (doc == NULL) {
        send_error(res, 400, "invalid json");
        return;
    }
    yyjson_val *body = yyjson_doc_get_root(doc);

    yyjson_val *title_value = yyjson_obj_get(body, "title");
    const char *title = yyjson_get_str(title_value);
    if (title_value != NULL && !todo_title_is_valid(title)) {
        yyjson_doc_free(doc);
        send_error(res, 400, "title must be 1-255 characters");
        return;
    }

    yyjson_val *done_value = yyjson_obj_get(body, "done");
    int done_storage = 0;
    const int *done = NULL;
    if (done_value != NULL) {
        done_storage = yyjson_get_bool(done_value);
        done = &done_storage;
    }

    Todo todo;
    int result = db_patch_todo(id, title, done, &todo);
    yyjson_doc_free(doc);
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
