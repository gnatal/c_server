#ifndef RESPONSE_H
#define RESPONSE_H

#include "app_types.h"

/* Sets the status code that res_send() will report. */
void res_status(Response *res, int status);

/*
 * Builds a full HTTP response (status line + headers + body) into the
 * connection's out_buf. This does no socket I/O itself - in an event-loop
 * server the socket might not be writable yet, so the event loop
 * (flush_connection) is what actually sends these bytes, possibly across
 * several EVFILT_WRITE turns. Header and body are combined into one buffer
 * so the eventual write() goes out as a single TCP segment (same reasoning
 * as the earlier writev() fix: two separate sends let Nagle + delayed ACK
 * stack up into a multi-ms stall per request).
 */
void res_send(Response *res, const char *body);

/* Same as res_send, but with a "Content-Type: application/json" header. */
void res_json(Response *res, const char *body);

#endif /* RESPONSE_H */
