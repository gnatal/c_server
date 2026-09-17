#ifndef STATIC_H
#define STATIC_H

#include <stddef.h>
#include "app_types.h"

/*
 * Derives the sanitized, relative subpath a static-file request maps to
 * under its mount's root directory. mount_pattern is a registered static
 * route's own pattern (Route.path, always "<prefix>" plus a trailing
 * wildcard segment - see app_serve_static, router.h) and req_path is the
 * request path being served (already URL-decoded by parse_http_request
 * before routing ever sees it - see lib/CLAUDE.md, "URL decoding"). Strips
 * the mount's literal prefix off req_path, then walks the remainder segment
 * by segment (split on '/') and rejects (-1) any segment that is exactly
 * ".." - this is the
 * primary path-traversal defense, and is pure/fully unit-testable without
 * touching a filesystem (static_serve_file below adds a filesystem-level
 * check on top of this, since a symlink inside the mount's root can point
 * outside it without any ".." ever appearing in the request path). Also
 * rejects (-1) a req_path that doesn't actually start with the mount's
 * prefix, a remainder that resolves to nothing (a request for the mount
 * root itself, which a trailing "*" route never matches anyway - see
 * "Route wildcards", lib/CLAUDE.md), or a sanitized result that doesn't fit
 * out_size. On success (0), out holds the relative path (no leading slash)
 * to join onto the mount's root directory.
 */
int static_resolve_relative_path(const char *mount_pattern, const char *req_path, char *out, size_t out_size);

/*
 * Pure file-extension -> MIME type lookup, case-insensitive on the
 * extension. Returns "application/octet-stream" for any extension not in
 * the (small, demo-sized) table, or for a path with no extension at all.
 */
const char *static_mime_type(const char *path);

/*
 * Resolves req->path against route->static_root (an absolute, canonical
 * directory set by app_serve_static, router.h) and sends the matching file
 * as the response:
 *   - 403 if static_resolve_relative_path rejects the path, or if the path
 *     resolves (via realpath) to somewhere outside static_root even after
 *     that check passes (a symlink escape).
 *   - 404 if nothing exists there, or if it resolves to a directory with no
 *     index.html inside it, or to something that isn't a regular file
 *     (device node, FIFO, ...). There is no directory listing.
 *   - 500 if the file exceeds MAX_STATIC_FILE_SIZE (app_types.h) or a read
 *     fails partway through.
 *   - 200 with the file's bytes (via res_send_bytes, response.h - not
 *     res_send, since a served file can contain embedded NUL bytes) and a
 *     Content-Type from static_mime_type otherwise.
 * Called from chain_next's final fallback (lib/middleware.c) in place of a
 * Handler - a static route has none, see Route.static_root (app_types.h).
 */
void static_serve_file(const Route *route, const Request *req, Response *res);

#endif /* STATIC_H */
