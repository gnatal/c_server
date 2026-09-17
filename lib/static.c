#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

    res_status(res, 200);
    res_send_bytes(res, static_mime_type(resolved), buf, size);
    free(buf);
}
