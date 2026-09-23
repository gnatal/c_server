#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include "static.h"
#include "response.h"

int static_resolve_relative_path(const char *mount_pattern, const char *req_path, char *out, size_t out_size) {
    if (mount_pattern == NULL || req_path == NULL || out == NULL || out_size == 0) {
        return -1;
    }

    /* mount_pattern always ends in a trailing wildcard segment
     * (app_serve_static) - the mount's literal prefix is everything up to
     * and including that segment's leading '/'. */
    const size_t pattern_len = strlen(mount_pattern);
    const size_t prefix_len = (pattern_len > 0 && mount_pattern[pattern_len - 1] == '*')
        ? pattern_len - 1 : pattern_len;

    if (strncmp(req_path, mount_pattern, prefix_len) != 0) {
        return -1;
    }
    const char *remainder = req_path + prefix_len;
    if (remainder[0] == '/') {
        remainder++;
    }
    if (remainder[0] == '\0') {
        return -1;
    }

    char remainder_copy[512];
    const size_t remainder_len = strlen(remainder);
    if (remainder_len >= sizeof(remainder_copy)) {
        return -1;
    }
    memcpy(remainder_copy, remainder, remainder_len + 1);

    char *saveptr;
    char *tok = strtok_r(remainder_copy, "/", &saveptr);
    size_t offset = 0;
    int wrote_any = 0;
    out[0] = '\0';

    while (tok != NULL) {
        if (strcmp(tok, "..") == 0) {
            return -1;
        }
        const size_t tok_len = strlen(tok);
        const size_t needed = tok_len + (wrote_any ? 1 : 0);
        if (offset + needed >= out_size) {
            return -1;
        }
        if (wrote_any) {
            out[offset++] = '/';
        }
        memcpy(out + offset, tok, tok_len);
        offset += tok_len;
        out[offset] = '\0';
        wrote_any = 1;
        tok = strtok_r(NULL, "/", &saveptr);
    }

    /* Every segment was "." or otherwise skipped down to nothing (e.g. the
     * remainder was just "/" after prefix-stripping) - nothing to serve. */
    return wrote_any ? 0 : -1;
}

typedef struct {
    const char *ext;
    const char *mime;
} MimeEntry;

static const MimeEntry MIME_TABLE[] = {
    { ".html", "text/html" },
    { ".htm", "text/html" },
    { ".css", "text/css" },
    { ".js", "application/javascript" },
    { ".json", "application/json" },
    { ".png", "image/png" },
    { ".jpg", "image/jpeg" },
    { ".jpeg", "image/jpeg" },
    { ".gif", "image/gif" },
    { ".svg", "image/svg+xml" },
    { ".ico", "image/x-icon" },
    { ".txt", "text/plain" },
    { ".pdf", "application/pdf" },
    { ".woff", "font/woff" },
    { ".woff2", "font/woff2" },
    { ".mp4", "video/mp4" },
    { ".wasm", "application/wasm" },
};

const char *static_mime_type(const char *path) {
    const char *dot = strrchr(path, '.');
    if (dot == NULL) {
        return "application/octet-stream";
    }
    for (size_t i = 0; i < sizeof(MIME_TABLE) / sizeof(MIME_TABLE[0]); i++) {
        if (strcasecmp(dot, MIME_TABLE[i].ext) == 0) {
            return MIME_TABLE[i].mime;
        }
    }
    return "application/octet-stream";
}

/* path starts with root at a '/' boundary (or equals it exactly) - the
 * containment check that catches a symlink inside root escaping it, on top
 * of static_resolve_relative_path's textual ".." rejection above. */
static int path_is_within_root(const char *path, const char *root) {
    const size_t root_len = strlen(root);
    if (strncmp(path, root, root_len) != 0) {
        return 0;
    }
    return path[root_len] == '\0' || path[root_len] == '/';
}

/*
 * realpath()s candidate and, if that succeeds, verifies containment within
 * root and stat()s the result. Returns 1 (resolved_out/st filled in) on
 * success, 0 if candidate doesn't exist (or any other realpath/stat
 * failure - deliberately not distinguished, so a permission error looks
 * the same as "not found" rather than confirming the path exists), -1 if
 * it resolves outside root.
 */
static int resolve_and_stat(const char *root, const char *candidate, char *resolved_out, struct stat *st) {
    if (realpath(candidate, resolved_out) == NULL) {
        return 0;
    }
    if (!path_is_within_root(resolved_out, root)) {
        return -1;
    }
    if (stat(resolved_out, st) != 0) {
        return 0;
    }
    return 1;
}

/*
 * A small in-memory cache of recently served static files, so the common case (a handful of
 * assets requested repeatedly) skips realpath()/stat()/fopen()/fread()/malloc()/free() on every request
 * instead of paying for all of it every time - MEASURED (improvements.md) at 6.6x slower than an
 * equivalent in-memory response for a 52-byte file. Keyed by the *candidate* path (static_root + the
 * already-sanitized subpath) rather than the realpath()-resolved one: that is what lets a hit within
 * STATIC_CACHE_REVALIDATE_SECONDS of its last check skip realpath()/stat() too, not just the read - see
 * cache_lookup_fresh below, and lib/CLAUDE.md's "Static" note for the resulting trade-off (a symlink
 * swapped in-place can serve stale content for up to that window). Entries are process-lifetime, shared
 * across every mount (two different mounts can never resolve to the same candidate string, each being
 * rooted under its own canonical static_root) and bounded by STATIC_CACHE_MAX_ENTRIES *
 * STATIC_CACHE_MAX_ENTRY_BYTES <= STATIC_CACHE_MAX_TOTAL_BYTES (app_types.h; asserted below), so there is
 * no separate per-insert total-bytes accounting to get wrong - capping entry count alone caps total bytes.
 */
typedef struct {
    char *path;                /* malloc'd; the candidate string this entry was cached under */
    unsigned char *data;       /* malloc'd file bytes; NULL only when size == 0 */
    size_t size;
    time_t mtime;               /* st_mtime when data was read, for change detection on revalidation */
    time_t last_checked;        /* wall-clock time data/mtime were last confirmed still current */
    const char *content_type;   /* points into MIME_TABLE's string literals above; never freed */
} StaticCacheEntry;

_Static_assert((size_t)STATIC_CACHE_MAX_ENTRIES * STATIC_CACHE_MAX_ENTRY_BYTES <= STATIC_CACHE_MAX_TOTAL_BYTES,
               "static cache: entries * per-entry cap must not exceed the total cap (cache_insert relies on this)");

static StaticCacheEntry g_static_cache[STATIC_CACHE_MAX_ENTRIES];
static int g_static_cache_count = 0;

static char *dup_path(const char *s) {
    const size_t len = strlen(s) + 1;
    char *out = malloc(len);
    if (out != NULL) {
        memcpy(out, s, len);
    }
    return out;
}

static StaticCacheEntry *cache_find(const char *path) {
    for (int i = 0; i < g_static_cache_count; i++) {
        if (strcmp(g_static_cache[i].path, path) == 0) {
            return &g_static_cache[i];
        }
    }
    return NULL;
}

/* Frees the least-recently-confirmed-fresh entry's owned memory to make room for a new one. */
static void cache_evict_stalest(void) {
    if (g_static_cache_count == 0) {
        return;
    }
    int oldest = 0;
    for (int i = 1; i < g_static_cache_count; i++) {
        if (g_static_cache[i].last_checked < g_static_cache[oldest].last_checked) {
            oldest = i;
        }
    }
    free(g_static_cache[oldest].path);
    free(g_static_cache[oldest].data);
    g_static_cache[oldest] = g_static_cache[g_static_cache_count - 1];
    g_static_cache_count--;
}

/*
 * A hit within STATIC_CACHE_REVALIDATE_SECONDS of its last check needs no filesystem call at all - the
 * fast path that closes most of the gap to an in-memory res_send_bytes response. NULL if there is no
 * entry for `path`, or its check is stale enough that the caller must fall back to resolve_and_stat.
 */
static const StaticCacheEntry *cache_lookup_fresh(const char *path, time_t now) {
    const StaticCacheEntry *entry = cache_find(path);
    if (entry != NULL && now - entry->last_checked < STATIC_CACHE_REVALIDATE_SECONDS) {
        return entry;
    }
    return NULL;
}

/*
 * Takes ownership of `data` (size bytes, malloc'd by the caller, or NULL iff size == 0) on success (0):
 * the cache now owns it and frees it on eviction, replacement or static_cache_clear. On failure (-1: the
 * file is too large for the cache, or the cache is out of memory for its own bookkeeping) the caller
 * keeps ownership and must free `data` itself. `content_type` must have static storage duration
 * (static_mime_type's return value qualifies); nothing here ever frees it.
 */
static int cache_insert(const char *path, unsigned char *data, size_t size, time_t mtime,
                         const char *content_type, time_t now) {
    if (size > STATIC_CACHE_MAX_ENTRY_BYTES) {
        return -1;
    }

    StaticCacheEntry *entry = cache_find(path);
    if (entry == NULL) {
        if (g_static_cache_count >= STATIC_CACHE_MAX_ENTRIES) {
            cache_evict_stalest();
        }
        char *key = dup_path(path);
        if (key == NULL) {
            return -1;
        }
        entry = &g_static_cache[g_static_cache_count++];
        entry->path = key;
        entry->data = NULL;
        entry->size = 0;
    }

    free(entry->data);
    entry->data = data;
    entry->size = size;
    entry->mtime = mtime;
    entry->last_checked = now;
    entry->content_type = content_type;
    return 0;
}

void static_cache_clear(void) {
    for (int i = 0; i < g_static_cache_count; i++) {
        free(g_static_cache[i].path);
        free(g_static_cache[i].data);
    }
    g_static_cache_count = 0;
}

void static_serve_file(const Route *route, const Request *req, Response *res) {
    char subpath[PATH_MAX];
    if (static_resolve_relative_path(route->path, req->path, subpath, sizeof(subpath)) != 0) {
        res_status(res, 403);
        res_send(res, "Forbidden");
        return;
    }

    char candidate[PATH_MAX];
    int n = snprintf(candidate, sizeof(candidate), "%s/%s", route->static_root, subpath);
    if (n < 0 || (size_t)n >= sizeof(candidate)) {
        res_status(res, 500);
        res_send(res, "Internal Server Error");
        return;
    }

    const time_t now = time(NULL);
    const StaticCacheEntry *fresh = cache_lookup_fresh(candidate, now);
    if (fresh != NULL) {
        res_status(res, 200);
        res_send_bytes(res, fresh->content_type, fresh->data, fresh->size);
        return;
    }

    char resolved[PATH_MAX];
    struct stat st;
    int rc = resolve_and_stat(route->static_root, candidate, resolved, &st);
    if (rc == -1) {
        res_status(res, 403);
        res_send(res, "Forbidden");
        return;
    }
    if (rc == 0) {
        res_status(res, 404);
        res_send(res, "Not Found");
        return;
    }

    if (S_ISDIR(st.st_mode)) {
        /* Express's default directory-index behavior - one retry against
         * "<resolved>/index.html", through the same containment check.
         * Never a directory listing. */
        char index_candidate[PATH_MAX];
        n = snprintf(index_candidate, sizeof(index_candidate), "%s/index.html", resolved);
        char index_resolved[PATH_MAX];
        if (n < 0 || (size_t)n >= sizeof(index_candidate) ||
            resolve_and_stat(route->static_root, index_candidate, index_resolved, &st) != 1 ||
            !S_ISREG(st.st_mode)) {
            res_status(res, 404);
            res_send(res, "Not Found");
            return;
        }
        strncpy(resolved, index_resolved, sizeof(resolved) - 1);
        resolved[sizeof(resolved) - 1] = '\0';
    } else if (!S_ISREG(st.st_mode)) {
        res_status(res, 404);
        res_send(res, "Not Found");
        return;
    }

    if ((size_t)st.st_size > (size_t)MAX_STATIC_FILE_SIZE) {
        fprintf(stderr, "static_serve_file: \"%s\" exceeds MAX_STATIC_FILE_SIZE, refusing to serve\n", resolved);
        res_status(res, 500);
        res_send(res, "Internal Server Error");
        return;
    }

    /* Past realpath()/stat(): a cache entry for this candidate is either absent, or was last confirmed
     * more than STATIC_CACHE_REVALIDATE_SECONDS ago. If the file is unchanged since then, reuse its
     * cached bytes instead of paying for fopen/fread again - the common case once a server has been up
     * for more than a second: one stat per file per second, not one full read. */
    StaticCacheEntry *existing = cache_find(candidate);
    if (existing != NULL && existing->mtime == st.st_mtime && existing->size == (size_t)st.st_size) {
        existing->last_checked = now;
        res_status(res, 200);
        res_send_bytes(res, existing->content_type, existing->data, existing->size);
        return;
    }

    FILE *f = fopen(resolved, "rb");
    if (f == NULL) {
        res_status(res, 404);
        res_send(res, "Not Found");
        return;
    }

    const size_t size = (size_t)st.st_size;
    unsigned char *buf = NULL;
    if (size > 0) {
        buf = malloc(size);
        if (buf == NULL) {
            fclose(f);
            res_status(res, 500);
            res_send(res, "Internal Server Error");
            return;
        }
        const size_t read_bytes = fread(buf, 1, size, f);
        fclose(f);
        if (read_bytes != size) {
            free(buf);
            res_status(res, 500);
            res_send(res, "Internal Server Error");
            return;
        }
    } else {
        fclose(f);
    }

    const char *content_type = static_mime_type(resolved);
    res_status(res, 200);
    res_send_bytes(res, content_type, buf, size);

    /* cache_insert takes ownership of buf on success (0); on failure (too big to cache, or out of
     * memory for the cache's own bookkeeping) it leaves buf untouched, so free it ourselves based on
     * the return value - never a double free, never a leak either way. */
    if (cache_insert(candidate, buf, size, st.st_mtime, content_type, now) != 0) {
        free(buf);
    }
}
