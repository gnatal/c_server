#ifndef HANDLERS_H
#define HANDLERS_H

#include "app_types.h"

void handler_home(const Request *req, Response *res);
void handler_get_user(const Request *req, Response *res);
void handler_echo(const Request *req, Response *res);
void handler_echo_json(const Request *req, Response *res);
void handler_api_status(const Request *req, Response *res);
void handler_login(const Request *req, Response *res);
void handler_whoami(const Request *req, Response *res);
void handler_logout(const Request *req, Response *res);
void handler_update_user(const Request *req, Response *res);
void handler_patch_user(const Request *req, Response *res);
void handler_delete_user(const Request *req, Response *res);
void handler_search(const Request *req, Response *res);
void handler_files(const Request *req, Response *res);
void handler_upload(const Request *req, Response *res);

#endif /* HANDLERS_H */
