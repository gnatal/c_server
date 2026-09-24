#ifndef STATIC_H
#define STATIC_H

#include <stddef.h>
#include <time.h>
#include "app_types.h"

/*
 * Derives the sanitized, relative subpath a static-file request maps to
 * under its mount's root directory. mount_pattern is a registered static
 * route's own pattern (Route.path, always "<prefix>" plus a trailing
 * wildcard segment - see app_serve_static, router.h) and req_path is the
 * request path being served (already URL-decoded by parse_http_request
 * before routing ever sees it - see lib/CLAUDE.md, "Behavior reference, Request parsing"). Strips
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
 * "Behavior reference, Routing", lib/CLAUDE.md), or a sanitized result that doesn't fit
 * out_size. Returns -2 for a segment starting with '.' (".env", ".git/config",
 * "a/.htpasswd"): dotfiles are never served, the serve-static default.
 * On success (0), out holds the relative path (no leading slash)
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
 *   - 404 for any path segment starting with '.' (static_resolve_relative_path's -2),
 *     whether or not the file exists, so a dotfile's existence is not revealed.
 *   - 404 if nothing exists there, or if it resolves to a directory with no
 *     index.html inside it, or to something that isn't a regular file
 *     (device node, FIFO, ...). There is no directory listing.
 *   - 500 if the file exceeds MAX_STATIC_FILE_SIZE (app_types.h) or a read
 *     fails partway through.
 *   - Every answer carries X-Content-Type-Options: nosniff.
 *   - 200 with a Content-Type from static_mime_type otherwise. A file up to
 *     STATIC_CACHE_MAX_ENTRY_BYTES is read whole into a cached SharedBody and
 *     sent with res_send_shared: the connection pins the cache's bytes and
 *     writes them from there, never copying them into the arena. A larger
 *     one is streamed from disk with res_send_file (response.h), never held
 *     in memory. 404 if it vanished between the stat and that open.
 * Called from chain_next's final fallback (lib/middleware.c) in place of a
 * Handler - a static route has none, see Route.static_root (app_types.h).
 *
 * Performance: files up to STATIC_CACHE_MAX_ENTRY_BYTES (app_types.h) are cached in memory after
 * their first successful read. A request within STATIC_CACHE_REVALIDATE_SECONDS of the last check for
 * the same path is served straight from the cache with no filesystem call at all (not even realpath or
 * stat); past that window, one stat confirms the file is unchanged (by size and mtime) before either
 * reusing the cached bytes or re-reading it. See lib/CLAUDE.md, "Static", for the resulting trade-off.
 */
void static_serve_file(const Route *route, const Request *req, Response *res);

/* One cached file (static.c's process-lifetime cache, see static_serve_file). */
typedef struct {
    char *path;                /* malloc'd; the candidate string this entry was cached under */
    SharedBody *body;          /* the file's bytes; the cache holds one reference, a response in flight may hold more */
    time_t mtime;              /* st_mtime when body was read, for change detection on revalidation */
    time_t last_checked;       /* wall-clock time body/mtime were last confirmed still current */
    const char *content_type;  /* points into static.c's MIME table string literals; never freed */
} StaticCacheEntry;

/*
 * Frees every entry in the static-file cache described above (static_serve_file): drops the cache's
 * reference to each body (one still being sent survives until its connection is done with it). The cache is
 * process-lifetime and shared across every mount, so this is for tests that need a clean slate between
 * runs and for an application that wants to force a reload (e.g. after redeploying static assets)
 * without restarting the worker; nothing in the engine itself calls it.
 */
void static_cache_clear(void);

#endif /* STATIC_H */
