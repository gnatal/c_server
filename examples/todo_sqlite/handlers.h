#ifndef HANDLERS_H
#define HANDLERS_H

#include "app_types.h"

/* Serves the Todo UI (app/public/index.html) via res_send_file. */
void handler_home(const Request *req, Response *res);

/* GET /api/todos[?done=true|false] - list all todos, optionally filtered. */
void handler_list_todos(const Request *req, Response *res);

/* GET /api/todos/:id */
void handler_get_todo(const Request *req, Response *res);

/* POST /api/todos - body: {"title": "..."} */
void handler_create_todo(const Request *req, Response *res);

/* PUT /api/todos/:id - full replace, body: {"title": "...", "done": bool} */
void handler_replace_todo(const Request *req, Response *res);

/* PATCH /api/todos/:id - partial update, body: {"title"?: "...", "done"?: bool} */
void handler_patch_todo(const Request *req, Response *res);

/* DELETE /api/todos/:id */
void handler_delete_todo(const Request *req, Response *res);

#endif /* HANDLERS_H */
