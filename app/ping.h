#ifndef PING_H
#define PING_H

#include "app_types.h"

/* GET /ping - answers 200 "pong" (text/plain). Touches no database and builds no JSON, so it
 * measures the engine's connection handling on its own: accept, keep-alive, close. Kept in its
 * own file so tests/test_ping.c can link it without SQLite. */
void handler_ping(const Request *req, Response *res);

#endif /* PING_H */
