#ifndef RESPONSE_H
#define RESPONSE_H

#include <stddef.h>
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

/*
 * Same as res_send, but for a raw byte buffer of known length rather than a
 * NUL-terminated C string - res_send/res_json compute Content-Length via
 * strlen(body), which would silently truncate a body containing an embedded
 * NUL byte (e.g. a served binary file, lib/static.h: static_serve_file).
 * data may be NULL only if len is 0.
 */
void res_send_bytes(Response *res, const char *content_type, const unsigned char *data, size_t len);

/*
 * Sets the "Location" header to location and sends a short text/plain body
 * naming the redirect target, mirroring Express's res.redirect([status,]
 * path). status must be a 3xx code; 0 defaults to 302 Found (Express's own
 * default) since C has no optional arguments to omit it with.
 */
void res_redirect(Response *res, int status, const char *location);

/*
 * Appends a "Set-Cookie" response header for name=value, formatted per RFC
 * 6265 with whatever attributes options requests (CookieOptions,
 * app_types.h) - options may be NULL, meaning every attribute defaults (a
 * session cookie scoped to Path=/, no Domain/HttpOnly/Secure/SameSite).
 * Unlike res_set_header, repeated calls (even with the same name) always
 * append a new Set-Cookie line rather than overwriting one - a response
 * can legitimately set more than one cookie, and res_set_cookie never
 * deduplicates by name (the browser tells cookies apart by name + Path +
 * Domain together, not by whichever Set-Cookie line came last). Bounded by
 * MAX_RESPONSE_COOKIES; extra calls past the cap are dropped (stderr
 * warning), same convention as res_set_header/MAX_RESPONSE_HEADERS. A
 * cookie that doesn't fit MAX_SET_COOKIE_LEN once formatted is dropped the
 * same way rather than being silently truncated into a malformed
 * Set-Cookie line.
 */
void res_set_cookie(Response *res, const char *name, const char *value, const CookieOptions *options);

/*
 * Expires an existing cookie immediately: shorthand for res_set_cookie
 * with Max-Age=0 - the value is irrelevant once a cookie is expired, so
 * this always sends an empty one. path must match whatever Path the
 * cookie was originally set with (defaults to "/", res_set_cookie's own
 * default) - a browser only clears a cookie whose Path (and Domain, not
 * offered here) matches exactly, it does not clear every cookie of that
 * name regardless of scope. Mirrors Express's res.clearCookie(name).
 */
void res_clear_cookie(Response *res, const char *name, const char *path);

/*
 * Emits a chunk of data to the client using HTTP/1.1 Transfer-Encoding: chunked.
 * On the first call to res_write (or res_end), the HTTP response headers
 * (including Transfer-Encoding: chunked, custom headers, and any staged Trailer
 * declarations) are serialized into conn->out_buf. Subsequent calls append
 * "<hex_len>\r\n<data>\r\n" chunks to conn->out_buf, growing it dynamically
 * up to MAX_BODY_SIZE. For HEAD requests, chunk data is suppressed.
 */
void res_write(Response *res, const char *data, size_t len);

/*
 * Sets a trailing response header to be emitted after the final chunk (0\r\n)
 * per RFC 7230 §4.1.2. Repeated calls with the same name (case-insensitive)
 * overwrite in place. Bounded by MAX_RESPONSE_TRAILERS.
 */
void res_set_trailer(Response *res, const char *name, const char *value);

/*
 * Terminates a chunked streaming response: emits headers if not yet sent,
 * writes the terminal 0\r\n chunk, serializes all staged trailers (followed by
 * the terminating blank line \r\n), and marks the stream as ended.
 */
void res_end(Response *res);

/*
 * Streams a static file directly from disk to the client in bounded STREAM_CHUNK_SIZE
 * (16KB) chunks through the event loop, without buffering the entire file into
 * RAM. Opens filepath, stats it, sends Content-Length and Content-Type headers,
 * and attaches the file descriptor to the Connection. Returns 0 on success,
 * or -1 if the file could not be opened or is not a regular file.
 */
int res_send_file(Response *res, const char *content_type, const char *filepath);

#endif /* RESPONSE_H */
