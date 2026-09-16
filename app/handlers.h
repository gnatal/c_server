#ifndef HANDLERS_H
#define HANDLERS_H

#include "app_types.h"

void handler_home(const Request *req, Response *res);
void handler_get_user(const Request *req, Response *res);
void handler_echo(const Request *req, Response *res);
void handler_echo_json(const Request *req, Response *res);

#endif /* HANDLERS_H */
