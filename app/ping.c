#include "ping.h"
#include "response.h"

void handler_ping(const Request *req, Response *res) {
    (void)req;
    res_send(res, "pong");
}
