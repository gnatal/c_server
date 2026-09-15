#ifndef HANDLERS_H
#define HANDLERS_H

#include "appTypes.h"

void handler_home(Request *req, Response *res);
void handler_get_user(Request *req, Response *res);
void handler_echo(Request *req, Response *res);
void handler_echo_json(Request *req, Response *res);

#endif /* HANDLERS_H */
