#ifndef RESPONSE_H
#define RESPONSE_H

#include "app_types.h"

/* Sets the status code that res_send() will report. */
void res_status(Response *res, int status);

/*
 * Sets a response header to be sent alongside res_send()/res_json()'s own
 * headers. A second call with the same name (case-insensitive) overwrites
 * the first, matching Express's res.set(). "Content-Length" and
 * "Connection" are reserved - the response layer computes those itself, so
 * attempts to set them are rejected (logged, not applied). "Content-Type"
 * may be overridden this way, which takes priority over res_send/res_json's
 * default content type.
 */
void res_set_header(Response *res, const char *name, const char *value);

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
