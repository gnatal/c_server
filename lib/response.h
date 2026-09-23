#ifndef RESPONSE_H
#define RESPONSE_H

#include <stddef.h>
#include "app_types.h"

/*
 * Building a response. A handler produces exactly one response through ONE of:
 *   res_send / res_json / res_send_bytes / res_redirect   (whole body at once)
 *   res_write ... res_end                                  (chunked streaming)
 *   res_send_file                                          (file streamed from disk)
 *   res_stream                                             (producer called by the event loop, M5)
 * Order: res_status and res_set_header / res_set_cookie first, then the sending call.
 * Nothing here touches the socket: the bytes are built into conn->out_buf and written later by the
 * event loop (flush_connection). A second res_send in the same request replaces the first.
 * Body strings are copied, the caller's buffers need not outlive the call.
 */

/* Prepares a Response for `conn`: status 200, no headers/cookies/trailers, not HEAD, not streaming.
 * The engine calls this before every dispatch; unit tests may call it instead of memset. */
void res_init(Response *res, Connection *conn);

/* Sets the status code (default 200). */
void res_status(Response *res, int status);

/*
 * Sets a response header. Same name (case-insensitive) overwrites. Content-Length and Connection are
 * computed by the engine and rejected here (logged). A custom Content-Type replaces the default one.
 * Limits: MAX_RESPONSE_HEADERS; name truncated to 63 chars, value to 255; excess is dropped (logged).
 * Safe with request data: an empty name, or any control character (CR, LF, NUL ...) in name or value,
 * drops the header (logged) instead of allowing header injection / response splitting.
 */
void res_set_header(Response *res, const char *name, const char *value);

/* Sends `body` (NUL-terminated) as text/plain. */
void res_send(Response *res, const char *body);

/* Sends `body` (NUL-terminated JSON text) as application/json. */
void res_json(Response *res, const char *body);

/* Sends `len` raw bytes (may contain NULs) with an explicit content type. data may be NULL only if len is 0. */
void res_send_bytes(Response *res, const char *content_type, const unsigned char *data, size_t len);

/* Sets Location and sends a short text body. `status` should be 3xx; 0 means 302. A location containing
 * control characters is refused with 500. It is NOT checked against open redirects: validate untrusted targets. */
void res_redirect(Response *res, int status, const char *location);

/*
 * Adds one Set-Cookie line (RFC 6265) per call; repeated calls with the same name add more lines.
 * options may be NULL: session cookie, Path=/. CookieOptions.max_age: > 0 lifetime in seconds,
 * 0 (zero value) session cookie, < 0 expire now. Limits: MAX_RESPONSE_COOKIES; a cookie longer than
 * MAX_SET_COOKIE_LEN once formatted is dropped (logged), never truncated.
 * Name, value, path and domain containing control characters or ';' (and names containing '=', ' ' or ',')
 * drop the cookie (logged): request data cannot inject attributes or headers.
 */
void res_set_cookie(Response *res, const char *name, const char *value, const CookieOptions *options);

/* Expires a cookie: empty value, Max-Age=0. `path` must equal the Path it was set with (NULL = "/"). */
void res_clear_cookie(Response *res, const char *name, const char *path);

/*
 * Chunked streaming for bodies of unknown size. The first res_write (or res_end) commits the status
 * line and headers with Transfer-Encoding: chunked, so set headers and trailers-to-declare first.
 * Chunks accumulate in conn->out_buf (limit MAX_BODY_SIZE + header) and nothing is sent until the
 * handler returns; the handler never blocks on the socket. For large or unbounded bodies use res_stream. HEAD requests get the headers only. res_end writes the last chunk plus any trailers.
 * res_set_trailer: same-name overwrites; Transfer-Encoding, Content-Length and Trailer are rejected;
 * MAX_RESPONSE_TRAILERS.
 */
void res_write(Response *res, const char *data, size_t len);
void res_set_trailer(Response *res, const char *name, const char *value);
void res_end(Response *res);

/*
 * Streams a file from disk in STREAM_CHUNK_SIZE pieces without loading it into memory. Sends
 * Content-Length and `content_type`. Returns 0, or -1 if the file cannot be opened or is not a regular
 * file (nothing has been sent: respond with an error yourself). Caller must have validated `filepath`
 * (this does no traversal checking; use app_serve_static for untrusted paths).
 */
int res_send_file(Response *res, const char *content_type, const char *filepath);

/*
 * Producer streaming (M5) for bodies too large or too slow to build inside the handler: large generated
 * downloads, server-sent events, long-poll. Commits the status line and headers (Transfer-Encoding:
 * chunked; set Content-Type etc. first) and returns; the event loop then calls
 * `producer(writer, ctx)` every time the previous output has drained to the socket, so memory stays at
 * one STREAM_CHUNK_SIZE buffer per connection however long the stream runs. See StreamProducer and the
 * STREAM_* return values in app_types.h. The producer runs outside the handler: it must not touch the
 * Request, Response or arena, only `ctx`. Trailers are not supported here (dropped, logged).
 * Ownership: on 0 the engine owns ctx and calls ctx_free(ctx) exactly once (ctx_free may be NULL);
 * a HEAD request gets the headers only and ctx_free runs before this returns. On -1 (headers already
 * sent, NULL producer, or the head did not fit) nothing was taken: the caller still owns ctx.
 */
int res_stream(Response *res, StreamProducer producer, void *ctx, StreamCtxFree ctx_free);

/*
 * For use inside a StreamProducer: appends `len` bytes as one chunk. Returns 0, or -1 with nothing
 * written when this turn's buffer is full (keep the data and return STREAM_MORE: you are called again
 * once it drains). A single write up to STREAM_WRITE_MAX always fits an empty buffer; a larger one
 * never fits, split it. len == 0 is a no-op (it would otherwise end the body).
 */
int stream_write(StreamWriter *out, const void *data, size_t len);

/* Engine-internal: detaches a producer stream from conn and calls its ctx_free once. No-op if none. */
void stream_release(Connection *conn);

#endif /* RESPONSE_H */
